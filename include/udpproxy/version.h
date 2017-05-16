#pragma once

#include <experimental/string_view>

namespace UdpProxy {

using namespace std::experimental::string_view_literals;

constexpr std::tuple<unsigned, unsigned, unsigned> VERSION {0, 1, 0};
constexpr std::experimental::string_view SERVER_NAME = "udpproxy 0.1.0"sv;
#define UDPPROXY_SERVER_NAME_DEFINE "udpproxy 0.1.0"

}
