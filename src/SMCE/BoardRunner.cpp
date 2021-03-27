/*
 *  BoardRunner.cpp
 *  Copyright 2021 ItJustWorksTM
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <boost/predef.h>
#include <SMCE/BoardRunner.hpp>

#if BOOST_OS_UNIX || BOOST_OS_MACOS
#include <csignal>
#elif BOOST_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")
extern "C" {
__declspec(dllimport) LONG NTAPI NtResumeProcess(HANDLE ProcessHandle);
__declspec(dllimport) LONG NTAPI NtSuspendProcess(HANDLE ProcessHandle);
}
#else
#error "Unsupported platform"
#endif

#include <atomic>
#include <ctime>
#include <string>
#include <type_traits>
#include <boost/process.hpp>
#include <SMCE/BoardConf.hpp>
#include <SMCE/BoardView.hpp>
#include <SMCE/ExecutionContext.hpp>
#include <SMCE/internal/SharedBoardData.hpp>
#include <SMCE/internal/utils.hpp>

using namespace std::literals;
namespace bp = boost::process;
namespace bip = boost::interprocess;

namespace smce {

enum class BoardRunner::Command {
    run,      // <==>
    stop,     // ==>
    suspend,  // ==>
    stop_ack, // <==
};

struct BoardRunner::Internal {
    static inline std::atomic_uint64_t last_id = std::time(nullptr);
    std::uint64_t sketch_id = ++last_id;
    SharedBoardData sbdata;
    bp::child sketch;
    bp::ipstream sketch_log;
    std::thread sketch_log_grabber;
};

BoardRunner::BoardRunner(ExecutionContext& ctx, std::function<void(int)> exit_notify) noexcept
    : m_exectx{ctx}
    , m_exit_notify{std::move(exit_notify)}
    , m_internal{std::make_unique<Internal>()}
{
    m_build_log.reserve(4096);
    m_runtime_log.reserve(4096);
}

BoardRunner::~BoardRunner() {
    if (m_internal) {
        auto& in = *m_internal;
        if (in.sketch.valid() && in.sketch.running())
            in.sketch.terminate();
        if (in.sketch_log_grabber.joinable())
            in.sketch_log_grabber.join();
    }

    if (!m_sketch_dir.empty()) {
        [[maybe_unused]] std::error_code ec;
        stdfs::remove_all(m_sketch_dir, ec);
    }
}

[[nodiscard]] BoardView BoardRunner::view() noexcept {
    if (m_status != Status::configured && m_status != Status::built && m_status != Status::running && m_status != Status::suspended)
        return {};
    return BoardView{*m_internal->sbdata.get_board_data()};
}

void BoardRunner::tick() noexcept {
   switch (m_status) {
   case Status::running:
   case Status::suspended:
       if (!m_internal->sketch.running()) {
           m_status = Status::stopped;
           if (m_exit_notify)
               m_exit_notify(m_internal->sketch.exit_code());
       }
   default:
       ;
   }
}

bool BoardRunner::reset() noexcept {
    switch (m_status) {
    case Status::running:
    case Status::suspended:
        return false;
    default:
        if (m_internal) {
            auto& in = *m_internal;
            if (in.sketch.valid() && in.sketch.running())
                in.sketch.terminate();
            if (in.sketch_log_grabber.joinable())
                in.sketch_log_grabber.join();
        }
        m_internal = std::make_unique<Internal>();
        if (!m_sketch_dir.empty())
            stdfs::remove_all(m_sketch_dir);
        m_sketch_path.clear();
        m_sketch_dir.clear();
        m_sketch_bin.clear();
        m_build_log.clear();
        m_runtime_log.clear();
        m_status = Status::clean;
        return true;
    }
}

bool BoardRunner::configure(std::string_view pp_fqbn, BoardConfig bconf) noexcept {
    if (!(m_status == Status::clean || m_status == Status::configured))
        return false;

    namespace bp = boost::process;

    m_internal->sbdata.configure("SMCE-Runner-" + std::to_string(m_internal->sketch_id), pp_fqbn, bconf);
    m_bconf = std::move(bconf);
    m_fqbn = pp_fqbn;
    m_status = Status::configured;
    return true;
}

bool BoardRunner::build(const stdfs::path& sketch_src, const SketchConfig& skonf) noexcept {
    const auto& res_path = m_exectx.resource_dir();
    const auto& cmake_path = m_exectx.cmake_path();

#if !BOOST_OS_WINDOWS
    const char* const generator_override = std::getenv("CMAKE_GENERATOR");
    const char* const generator = generator_override ? generator_override : (!bp::search_path("ninja").empty() ? "Ninja" : "");
#endif
    std::string dir_arg = "-DSMCE_DIR=" + res_path.string();
    std::string fqbn_arg = "-DSKETCH_FQBN=" + m_fqbn;
    std::string ident_arg = "-DSKETCH_IDENT=" + std::to_string(m_internal->sketch_id);
    std::string sketch_arg = "-DSKETCH_PATH=" + stdfs::absolute(sketch_src).generic_string();
    std::string pp_remote_libs_arg = "-DPREPROC_REMOTE_LIBS=";
    std::string cl_remote_libs_arg = "-DCOMPLINK_REMOTE_LIBS=";
    std::string cl_local_libs_arg = "-DCOMPLINK_LOCAL_LIBS=";
    std::string cl_patch_libs_arg = "-DCOMPLINK_PATCH_LIBS=";
    for (const auto& lib : skonf.preproc_libs) {
        std::visit(Visitor{
           [&](const SketchConfig::RemoteArduinoLibrary& lib){
               pp_remote_libs_arg += lib.name;
               if(!lib.version.empty())
                   pp_remote_libs_arg += '@' + lib.version;
             pp_remote_libs_arg += ';';
           },
           [](const auto&) {}
        }, lib);
    }

    for (const auto& lib : skonf.complink_libs) {
        std::visit(Visitor{
            [&](const SketchConfig::RemoteArduinoLibrary& lib){
                cl_remote_libs_arg += lib.name;
                if(!lib.version.empty())
                    cl_remote_libs_arg += '@' + lib.version;
                cl_remote_libs_arg += ';';
            },
            [&](const SketchConfig::LocalArduinoLibrary& lib){
                if(lib.patch_for.empty()) {
                    cl_local_libs_arg += lib.root_dir.string();
                    cl_local_libs_arg += ';';
                    return;
                }
                cl_remote_libs_arg += lib.patch_for;
                cl_remote_libs_arg += ' ';
                cl_patch_libs_arg += lib.root_dir.string();
                cl_patch_libs_arg += '|';
                cl_patch_libs_arg += lib.patch_for;
                cl_patch_libs_arg += ';';
            },
            [](const SketchConfig::FreestandingLibrary&) {}
        }, lib);
    }

    if(pp_remote_libs_arg.back() == ';') pp_remote_libs_arg.pop_back();
    if(cl_remote_libs_arg.back() == ';') cl_remote_libs_arg.pop_back();
    if(cl_local_libs_arg.back() == ';') cl_local_libs_arg.pop_back();
    if(cl_patch_libs_arg.back() == ';') cl_patch_libs_arg.pop_back();

    namespace bp = boost::process;
    bp::ipstream cmake_conf_out;
    auto cmake_config = bp::child(
        cmake_path,
#if !BOOST_OS_WINDOWS
        bp::env["CMAKE_GENERATOR"] = generator,
#endif
        std::move(ident_arg),
        std::move(dir_arg),
        std::move(fqbn_arg),
        std::move(sketch_arg),
        std::move(pp_remote_libs_arg),
        std::move(cl_remote_libs_arg),
        std::move(cl_local_libs_arg),
        std::move(cl_patch_libs_arg),
        "-P",
        res_path.string() + "/RtResources/SMCE/share/Scripts/ConfigureSketch.cmake",
        (bp::std_out & bp::std_err) > cmake_conf_out
    );

    {
        std::string line;
        int i = 0;
        while (std::getline(cmake_conf_out, line)) {
            if (!line.starts_with("-- SMCE: ")) {
                [[maybe_unused]] std::lock_guard lk{m_build_log_mtx};
                (m_build_log += line) += '\n';
                continue;
            }
            line.erase(0, line.find_first_of('"') + 1);
            line.pop_back();
            switch (i++) {
            case 0:
                m_sketch_dir = std::move(line);
                break;
            case 1:
                m_sketch_bin = std::move(line);
                break;
            default:
                assert(false);
            }
        }
    }

    cmake_config.join();
    if (cmake_config.native_exit_code() != 0)
        return false;

    m_sketch_path = sketch_src;

    return do_build();
}

bool BoardRunner::rebuild() noexcept {
    if (m_sketch_path.empty()
        || m_status == Status::running
        || m_status == Status::suspended)
        return false;

    m_internal->sbdata.reset();
    m_internal->sbdata.configure("SMCE-Runner-" + std::to_string(m_internal->sketch_id), m_fqbn, m_bconf);

    const auto& res_path = m_exectx.resource_dir();

    bp::ipstream cmake_conf_out;
    auto cmake_config = bp::child{
        m_exectx.cmake_path(),
        "-DSMCE_DIR=" + res_path.string(),
        "-DSKETCH_IDENT=" + std::to_string(m_internal->sketch_id),
        "-DSKETCH_FQBN=" + m_fqbn,
        "-DSKETCH_PATH=" + stdfs::absolute(m_sketch_path).generic_string(),
        "-P",
        res_path.string() + "/RtResources/SMCE/share/Scripts/ConfigureSketch.cmake",
        (bp::std_out & bp::std_err) > cmake_conf_out
    };

    for (std::string line; std::getline(cmake_conf_out, line);) {
        [[maybe_unused]] std::lock_guard lk{m_build_log_mtx};
        (m_build_log += line) += '\n';
    }

    cmake_config.join();
    if (cmake_config.native_exit_code() != 0)
        return false;

    return do_build();
}

bool BoardRunner::start() noexcept {
    if (m_status != Status::built)
        return false;

    m_internal->sketch =
        bp::child(
            bp::env["SEGNAME"] = "SMCE-Runner-" + std::to_string(m_internal->sketch_id),
            "\""+m_sketch_bin.string()+"\"",
            bp::std_out > bp::null,
            bp::std_err > m_internal->sketch_log
    );

    m_internal->sketch_log_grabber = std::thread{[&]{
        auto& stream = m_internal->sketch_log;
        std::string buf;
        buf.reserve(1024);
        while(!stream.fail()) {
            const int head = stream.get();
            if(head == std::remove_cvref_t<decltype(stream)>::traits_type::eof())
                break;
            buf.resize(stream.rdbuf()->in_avail());
            const auto count = stream.readsome(buf.data(), buf.size());
            [[maybe_unused]] std::lock_guard lk{m_runtime_log_mtx};
            const auto existing = m_runtime_log.size();
            m_runtime_log.resize(existing + count + 1);
            m_runtime_log[existing] = static_cast<char>(head);
            std::memcpy(m_runtime_log.data() + existing + 1, buf.data(), count);
        }
    }};

    m_status = Status::running;
    return true;
}

bool BoardRunner::suspend() noexcept {
    if (m_status != Status::running)
        return false;

#if defined(__unix__)
    ::kill(m_internal->sketch.native_handle(), SIGSTOP);
#elif defined(_WIN32) || defined(WIN32)
    NtSuspendProcess(m_internal->sketch.native_handle());
#endif

    m_status = Status::suspended;
    return true;
}

bool BoardRunner::resume() noexcept {
    if (m_status != Status::suspended)
        return false;

#if defined(__unix__)
    ::kill(m_internal->sketch.native_handle(), SIGCONT);
#elif defined(_WIN32) || defined(WIN32)
    NtResumeProcess(m_internal->sketch.native_handle());
#endif

    m_status = Status::running;
    return true;
}

bool BoardRunner::terminate() noexcept {
    if (m_status != Status::running && m_status != Status::suspended)
        return false;

    std::error_code ec;
    m_internal->sketch.terminate(ec);

    if (m_internal->sketch_log_grabber.joinable())
        m_internal->sketch_log_grabber.join();

    if (!ec)
        m_status = Status::stopped;
    return !ec;
}

/*
bool BoardRunner::stop() noexcept {
    if(m_status != Status::running)
        return false;

    auto& command = m_internal->command;
    command = Command::stop;
    command.notify_all();

    const auto val = command.wait(Command::stop);
    const bool success = val == Command::stop_ack;
    if(success)
        m_status = Status::stopped;

    return success;
}
*/

// FIXME
bool BoardRunner::stop() noexcept { return terminate(); }

bool BoardRunner::do_build() noexcept {
    bp::ipstream cmake_build_out;
    auto cmake_build = bp::child{
#if BOOST_OS_WINDOWS
        bp::env["MSBUILDDISABLENODEREUSE"] = "1", // MSBuild "feature" which uses your child processes as potential deamons, forever
#endif
        m_exectx.cmake_path(),
        "--build",
        (m_sketch_dir / "build").string(),
        (bp::std_out & bp::std_err) > cmake_build_out
    };

    for (std::string line; std::getline(cmake_build_out, line);) {
        [[maybe_unused]] std::lock_guard lk{m_build_log_mtx};
        (m_build_log += line) += '\n';
    }

    cmake_build.join();
    if (cmake_build.native_exit_code() != 0  || !stdfs::exists(m_sketch_bin))
        return false;

    m_status = Status::built;
    return true;
}

}
