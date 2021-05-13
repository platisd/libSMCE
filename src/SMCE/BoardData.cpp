/*
 *  BoardData.cpp
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

#include <SMCE/internal/BoardData.hpp>

#include <algorithm>
#include <SMCE/BoardConf.hpp>

namespace bip = boost::interprocess;

namespace smce {

BoardData::UartChannel::UartChannel(const ShmAllocator<void>& shm_valloc) : rx{shm_valloc}, tx{shm_valloc} {}

BoardData::DirectStorage::DirectStorage(const ShmAllocator<void>& shm_valloc) : root_dir{shm_valloc} {}

BoardData::FrameBuffer::FrameBuffer(const ShmAllocator<void>& shm_valloc) : data{shm_valloc} {}

BoardData::BoardData(
        const ShmAllocator<void>& shm_valloc,
        const BoardConfig& c) noexcept
    : pins{shm_valloc},
      uart_channels{shm_valloc},
      direct_storages{shm_valloc},
      frame_buffers{shm_valloc} {
    auto sorted_pins = c.pins;
    std::sort(sorted_pins.begin(), sorted_pins.end());

    pins.reserve(sorted_pins.size());
    for(const auto pin_id : sorted_pins) {
        auto& pin_obj = pins.emplace_back();
        pin_obj.id = pin_id;
    }

    for (const auto& gpio_driver : c.gpio_drivers) {
        const auto it = std::find(sorted_pins.begin(), sorted_pins.end(), gpio_driver.pin_id);
        if(it == sorted_pins.end())
            continue;
        const auto pin_idx = std::distance(sorted_pins.begin(), it);
        auto& pin = pins[pin_idx];
        if(gpio_driver.analog_driver) {
            auto& driver = gpio_driver.analog_driver.value();
            pin.can_analog_read = driver.board_read;
            pin.can_analog_write = driver.board_write;
        }
        if(gpio_driver.digital_driver) {
            auto& driver = gpio_driver.digital_driver.value();
            pin.can_digital_read = driver.board_read;
            pin.can_digital_write = driver.board_write;
        }
    }

    uart_channels.reserve(c.uart_channels.size());
    for(const auto& conf : c.uart_channels) {
        auto& data = uart_channels.emplace_back(shm_valloc);
        data.baud_rate = conf.baud_rate;
        data.rx_pin_override = conf.rx_pin_override;
        data.tx_pin_override = conf.tx_pin_override;
        data.max_buffered_rx = static_cast<std::uint16_t>(conf.rx_buffer_length);
        data.max_buffered_tx = static_cast<std::uint16_t>(conf.tx_buffer_length);
    }

    direct_storages.reserve(c.sd_cards.size());
    for(const auto& conf : c.sd_cards) {
        auto& data = direct_storages.emplace_back(shm_valloc);
        data.bus = BoardData::DirectStorage::Bus::SPI;
        data.accessor = conf.cspin;
        {
            auto rt_str = conf.root_dir.generic_string();
            data.root_dir.assign(std::string_view{rt_str});
        }
    }

    frame_buffers.reserve(c.frame_buffers.size());
    for(const auto& conf : c.frame_buffers) {
        auto& data = frame_buffers.emplace_back(shm_valloc);
        data.key = conf.key;
        data.direction = BoardData::FrameBuffer::Direction{static_cast<std::uint8_t>(conf.direction)};
    }
}

}