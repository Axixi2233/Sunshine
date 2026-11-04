/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Verify advertised surround mappings through the production DESCRIBE handler.
 */
#include "../tests_common.h"
#include <boost/asio.hpp>
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}
#include "src/rtsp.h"
#include "src/utility.h"

namespace rtsp_stream {
  struct rtsp_server_t;
  /** @brief Release a parsed RTSP message. */
  void free_msg(PRTSP_MESSAGE msg);
  using msg_t = util::safe_ptr<RTSP_MESSAGE, free_msg>;
  /** @brief Invoke the production DESCRIBE response handler. */
  void cmd_describe(rtsp_server_t *server, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);
}

TEST(RtspDescribeTest, AdvertisesCorrectSurroundChannelOrder) {
  using boost::asio::ip::tcp;
  boost::asio::io_context io;
  tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
  tcp::socket client {io};
  client.connect(acceptor.local_endpoint());
  auto server = acceptor.accept();
  rtsp_stream::launch_session_t session {};
  rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
  rtsp_stream::cmd_describe(nullptr, server, session, std::move(request));
  server.shutdown(tcp::socket::shutdown_send);
  std::string response;
  boost::system::error_code error;
  boost::asio::read(client, boost::asio::dynamic_buffer(response), error);
  ASSERT_EQ(error, boost::asio::error::eof);
  const std::string prefix = "a=fmtp:97 surround-params=";
  std::vector<std::string> layouts;
  for (auto pos = response.find(prefix); pos != std::string::npos; pos = response.find(prefix, pos + prefix.size())) {
    const auto end = response.find('\n', pos);
    layouts.push_back(response.substr(pos + prefix.size(), end - pos - prefix.size()));
  }
  EXPECT_EQ(layouts, (std::vector<std::string> {"21101", "21101", "642012453", "660012345", "85301245673", "88001234567"}));
}
