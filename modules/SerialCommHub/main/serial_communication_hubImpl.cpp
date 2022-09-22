// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2022 Pionix GmbH and Contributors to EVerest

#include "serial_communication_hubImpl.hpp"

#include <chrono>
#include <connection/exceptions.hpp>
#include <date/date.h>
#include <date/tz.h>
#include <fmt/core.h>
#include <modbus/exceptions.hpp>
#include <modbus/utils.hpp>

#include <typeinfo>

namespace module {
namespace main {

void serial_communication_hubImpl::init() {

    everest::connection::SerialDeviceConfiguration cfg(config.serial_port);
    cfg.set_sensible_defaults();
    cfg.set_baud_rate(baudrate_from_int(config.baudrate));
    cfg.set_stop_bits(everest::connection::SerialDeviceConfiguration::StopBits::One);
    cfg.set_data_bits(everest::connection::SerialDeviceConfiguration::DataBits::Bit_8);

    switch (config.parity) {
    case 0:
        cfg.set_parity(everest::connection::SerialDeviceConfiguration::Parity::None);
        break;
    case 1:
        cfg.set_parity(everest::connection::SerialDeviceConfiguration::Parity::Odd);
        break;
    case 2:
        cfg.set_parity(everest::connection::SerialDeviceConfiguration::Parity::Even);
        break;
    }

    serial_device = std::unique_ptr<everest::connection::SerialDevice>(new everest::connection::SerialDevice(cfg));
    modbus_connection =
        std::unique_ptr<everest::connection::RTUConnection>(new everest::connection::RTUConnection(*serial_device));
    modbus_client =
        std::unique_ptr<everest::modbus::ModbusRTUClient>(new everest::modbus::ModbusRTUClient(*modbus_connection));

    try {
        serial_device->open();
    } catch (const everest::connection::exceptions::tty::tty_error& e) {
        EVLOG_AND_THROW(Everest::EverestConfigError(fmt::format("Cannot open serial port {}.", config.serial_port)));
    }
}

void serial_communication_hubImpl::ready() {
}

// Commands

types::serial_comm_hub_requests::StatusCodeEnum
serial_communication_hubImpl::handle_send_raw(int& target_device_id,
                                              types::serial_comm_hub_requests::VectorUint8& data_raw, bool& add_crc16) {

    std::vector<uint8_t> message;
    // first byte is device id
    message.push_back((uint8_t)target_device_id);
    // the next bytes are just the raw message
    append_array<uint8_t, int>(message, data_raw.data);

    // do we need to add CRC at the end?
    if (add_crc16) {
        const uint16_t crc_sum = everest::modbus::utils::calcCRC_16_ANSI(message.data(), message.size());
        const uint8_t crc_low = (uint8_t)(crc_sum >> 8);
        const uint8_t crc_high = (uint8_t)(crc_sum & 0x00FF);

        message.push_back(crc_low);
        message.push_back(crc_high);
    }

    int bytes_written = 0;
    {
        // write to device
        std::scoped_lock lock(serial_mutex);

        bytes_written = serial_device->write(message.data(), message.size());
        serial_device->drain();
    }

    // check if all bytes were written
    if (bytes_written != message.size()) {
        return types::serial_comm_hub_requests::StatusCodeEnum::TxFailed;
    } else {
        return types::serial_comm_hub_requests::StatusCodeEnum::Success;
    }
}

types::serial_comm_hub_requests::ResultRaw serial_communication_hubImpl::handle_send_raw_wait_reply(
    int& target_device_id, types::serial_comm_hub_requests::VectorUint8& data_raw, bool& add_crc16) {

    types::serial_comm_hub_requests::ResultRaw status;

    std::vector<uint8_t> message;
    // first byte is device id
    message.push_back((uint8_t)target_device_id);
    // the next bytes are just the raw message
    append_array<uint8_t, int>(message, data_raw.data);

    // do we need to add CRC at the end?
    if (add_crc16) {
        const uint16_t crc_sum = everest::modbus::utils::calcCRC_16_ANSI(message.data(), message.size());
        const uint8_t crc_low = (uint8_t)(crc_sum >> 8);
        const uint8_t crc_high = (uint8_t)(crc_sum & 0x00FF);

        message.push_back(crc_low);
        message.push_back(crc_high);
    }

    int bytes_received = 0;
    uint8_t receive_buffer[everest::modbus::consts::rtu::MAX_REGISTER_PER_MESSAGE * 2];

    {
        std::scoped_lock lock(serial_mutex);

        // Write command
        int bytes_written = serial_device->write(message.data(), message.size());
        if (bytes_written != message.size()) {
            status.status_code = types::serial_comm_hub_requests::StatusCodeEnum::TxFailed;
            return status;
        }
        serial_device->drain();

        // Receive reply with timeout
        bytes_received = serial_device->read(receive_buffer, sizeof(receive_buffer));
    }

    // check if timed out
    if (bytes_received <= 0) {
        status.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxTimeout;
        return status;
    }

    // enough bytes received?
    if (bytes_received < 3) {
        status.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxBrokenReply;
        return status;
    }

    // check if from correct device
    if (target_device_id != receive_buffer[0]) {
        status.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxWrongDevice;
        return status;
    }

    // check if CRC is correct
    if (add_crc16 && !check_crc(receive_buffer, bytes_received)) {
        status.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxCRCMismatch;
        return status;
    }

    // All checks succeeded, return message
    status.status_code = types::serial_comm_hub_requests::StatusCodeEnum::Success;
    status.value = std::vector<int>(receive_buffer + 1, receive_buffer + bytes_received - 2);

    return status;
}

types::serial_comm_hub_requests::Result
serial_communication_hubImpl::handle_modbus_read_holding_registers(int& target_device_id, int& first_register_address,
                                                                   int& num_registers_to_read) {
    std::vector<std::uint8_t> response;
    types::serial_comm_hub_requests::Result result;
    result.status_code = types::serial_comm_hub_requests::StatusCodeEnum::ModbusException;

    {
        std::scoped_lock lock(serial_mutex);

        bool done{false};
        uint8_t retry_counter{this->num_resends_on_error};
        do {
            if (retry_counter < 1) {
                return result;
            }

            EVLOG_debug << fmt::format("Try {} Call modbus_client->read_holding_register(id {} addr {} len {})",
                                       (int)num_resends_on_error, (uint8_t)target_device_id,
                                       (uint16_t)first_register_address, (uint16_t)num_registers_to_read);

            try {
                response = modbus_client->read_holding_register(
                    (uint8_t)target_device_id, (uint16_t)first_register_address, (uint16_t)num_registers_to_read, true);

            } catch (const everest::modbus::exceptions::unmatched_response& e) {
                EVLOG_error << "Unmatched response error: " << e.what() << "\n";
                result.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxWrongDevice;
                retry_counter--;
                continue;
            } catch (const everest::modbus::exceptions::checksum_error& e) {
                EVLOG_error << "Checksum error: " << e.what() << "\n";
                result.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxCRCMismatch;
                retry_counter--;
                continue;
            } catch (const everest::modbus::exceptions::empty_response& e) {
                EVLOG_error << "Empty response error: " << e.what() << "\n";
                result.status_code = types::serial_comm_hub_requests::StatusCodeEnum::RxBrokenReply;
                retry_counter--;
                continue;
            } catch (const everest::modbus::exceptions::modbus_exception& e) {
                EVLOG_error << "Modbus exception: " << e.what() << "\n";
                result.status_code = types::serial_comm_hub_requests::StatusCodeEnum::ModbusException;
                retry_counter--;
                continue;
            }
            done = true;
        } while (!done);
    }

    EVLOG_debug << fmt::format("Process response (size {})", (int)response.size());
    // process response
    if (response.size() > 1) {
        result.status_code = types::serial_comm_hub_requests::StatusCodeEnum::Success;
        result.value = toLittleEndianInt(response);
    }
    return result;
}

types::serial_comm_hub_requests::StatusCodeEnum serial_communication_hubImpl::handle_modbus_write_multiple_registers(
    int& target_device_id, int& first_register_address, types::serial_comm_hub_requests::VectorUint16& data_raw) {
    std::vector<uint16_t> raw_message;
    append_array<uint16_t, int>(raw_message, data_raw.data);

    // pack vector into correct endianess modbus payload container
    everest::modbus::ModbusDataContainerUint16 payload(everest::modbus::ByteOrder::LittleEndian, raw_message);

    std::vector<std::uint8_t> response;
    types::serial_comm_hub_requests::StatusCodeEnum result;

    {
        std::scoped_lock lock(serial_mutex);

        bool done{false};
        uint8_t retry_counter{this->num_resends_on_error};
        do {
            if (retry_counter < 1) {
                return result;
            }

            EVLOG_debug << fmt::format("Try {} Call modbus_client->write_multiple_registers(id {} addr {} len {})",
                                       (int)num_resends_on_error, (uint8_t)target_device_id,
                                       (uint16_t)first_register_address, (uint16_t)payload.size());

            try {
                response =
                    modbus_client->write_multiple_registers((uint8_t)target_device_id, (uint16_t)first_register_address,
                                                            (uint16_t)payload.size(), payload, true);
            } catch (const everest::modbus::exceptions::unmatched_response& e) {
                EVLOG_error << "Unmatched response error: " << e.what() << "\n";
                result = types::serial_comm_hub_requests::StatusCodeEnum::RxWrongDevice;
                retry_counter--;
                continue;
            } catch (const everest::modbus::exceptions::checksum_error& e) {
                EVLOG_error << "Checksum error: " << e.what() << "\n";
                result = types::serial_comm_hub_requests::StatusCodeEnum::RxCRCMismatch;
                retry_counter--;
                continue;
            } catch (const everest::modbus::exceptions::empty_response& e) {
                EVLOG_error << "Empty response error: " << e.what() << "\n";
                result = types::serial_comm_hub_requests::StatusCodeEnum::RxBrokenReply;
                retry_counter--;
                continue;
            } catch (const everest::modbus::exceptions::modbus_exception& e) {
                EVLOG_error << "Modbus exception: " << e.what() << "\n";
                result = types::serial_comm_hub_requests::StatusCodeEnum::ModbusException;
                retry_counter--;
                continue;
            }
            done = true;
        } while (!done);
    }

    EVLOG_debug << fmt::format("Done writing");

    return types::serial_comm_hub_requests::StatusCodeEnum::Success;
}

// Helper functions

std::vector<uint16_t> serial_communication_hubImpl::toLittleEndian16(const std::vector<uint8_t> response) {
    std::vector<uint16_t> r;

    for (auto it = response.begin(); it != response.end();) {
        uint8_t hi = *it++;
        if (it == response.end())
            break;
        uint8_t lo = *it++;
        r.push_back((uint16_t)(hi << 8) | lo);
    }
    return r;
}

std::vector<int> serial_communication_hubImpl::toLittleEndianInt(const std::vector<uint8_t> response) {
    std::vector<int> r;
    uint8_t hi, lo;
    for (auto it = response.begin(); it != response.end();) {
        hi = *it++;
        if (it == response.end())
            break;
        lo = *it++;
        r.push_back((uint16_t)((hi << 8) | lo));
    }
    return r;
}

everest::connection::SerialDeviceConfiguration::BaudRate serial_communication_hubImpl::baudrate_from_int(int baudrate) {
    switch (baudrate) {
    case 1200:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_1200;
        break;
    case 2400:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_2400;
        break;
    case 4800:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_4800;
        break;
    case 9600:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_9600;
        break;
    case 19200:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_19200;
        break;
    case 38400:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_38400;
        break;
    case 57600:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_57600;
        break;
    case 115200:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_115200;
        break;
    case 230400:
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_230400;
        break;
    default:
        EVLOG_error << "Error setting up connection to serialport: Undefined baudrate! Defaulting to 9600.\n";
        return everest::connection::SerialDeviceConfiguration::BaudRate::Baud_9600;
        break;
    }
}

template <typename T, typename U>
void serial_communication_hubImpl::append_array(std::vector<T>& m, const std::vector<U>& a) {
    for (auto it = a.begin(); it != a.end(); ++it)
        m.push_back(*it);
}

bool serial_communication_hubImpl::check_crc(const uint8_t* buffer, size_t len) {
    if (len <= 2)
        return false;
    // check crc
    const uint16_t crc_sum = everest::modbus::utils::calcCRC_16_ANSI(buffer, len - 2);
    const uint8_t crc_low = (uint8_t)(crc_sum >> 8);
    const uint8_t crc_high = (uint8_t)(crc_sum & 0x00FF);

    if (crc_low == buffer[len - 2] && crc_high == buffer[len - 1])
        return true;
    else
        return false;
}

} // namespace main
} // namespace module