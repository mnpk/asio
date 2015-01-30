#include <boost/asio.hpp>
#include <boost/tokenizer.hpp>
#include <regex>
#include <unordered_map>
#include <thread>
#include <memory>
#include <iostream>
#include "json.hpp"

namespace http {

using nlohmann::json;
static std::unordered_map<int, std::string> status_codes = {
    {200, "HTTP/1.1 200 OK\r\n"},
    {201, "HTTP/1.1 201 Created\r\n"},
    {202, "HTTP/1.1 202 Accepted\r\n"},
    {204, "HTTP/1.1 204 No Content\r\n"},

    {300, "HTTP/1.1 300 Multiple Choices\r\n"},
    {301, "HTTP/1.1 301 Moved Permanently\r\n"},
    {302, "HTTP/1.1 302 Moved Temporarily\r\n"},
    {304, "HTTP/1.1 304 Not Modified\r\n"},

    {400, "HTTP/1.1 400 Bad Request\r\n"},
    {401, "HTTP/1.1 401 Unauthorized\r\n"},
    {403, "HTTP/1.1 403 Forbidden\r\n"},
    {404, "HTTP/1.1 404 Not Found\r\n"},

    {500, "HTTP/1.1 500 Internal Server Error\r\n"},
    {501, "HTTP/1.1 501 Not Implemented\r\n"},
    {502, "HTTP/1.1 502 Bad Gateway\r\n"},
    {503, "HTTP/1.1 503 Service Unavailable\r\n"},
};

static std::string seperator = ": ";
static std::string crlf = "\r\n";

struct request {
  std::string method, path, http_version;
  std::istream content;
  std::unordered_map<std::string, std::string> headers;
  std::vector<std::string> params;
  std::smatch path_match;
  request() : content(&buf) {}
  boost::asio::streambuf buf;
};

struct response {
  response() {}
  response(const char* c) : content(c) {}
  response(const std::string& c) : content(c) {}
  response(int c) : code(c) {}
  response(json j) : content(j.dump()) {
    headers.erase("content-type");
    headers.emplace("content-type", "application/json");
  }
  response& commit() {
    if (code >= 400 && content.empty()) {
      content = status_codes[code].substr(9);
    }
    if (false == content.empty()) {
      headers.erase("content-length");
      headers.emplace("content-length", std::to_string(content.length()));
    }
    return *this;
  }

  int code{200};
  std::string content;
  std::unordered_map<std::string, std::string> headers;
};

std::ostream& operator<<(std::ostream& s, const response& res) {
  s << status_codes.find(res.code)->second;
  for (const auto& h : res.headers) {
    s << h.first << seperator << h.second << crlf;
  }
  s << crlf;
  if (!res.content.empty()) s << res.content;
  return s;
}

typedef boost::asio::ip::tcp::socket socket;
typedef std::map<
    std::string,
    std::unordered_map<std::string,
                       std::function<response(std::shared_ptr<request>)>>>
    resource_t;

struct server {
  boost::asio::io_service the_io_service;
  std::unique_ptr<boost::asio::ip::tcp::endpoint> endpoint;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
  std::vector<std::thread> workers;
  size_t num_threads{1};
  size_t timeout_req{5};
  size_t timeout_content{300};
  uint16_t port_num{18080};
  resource_t route;
  std::vector<typename resource_t::iterator> resources;

  server& port(uint16_t p) {
    port_num = p;
    endpoint.reset(
        new boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), p));
    acceptor.reset(
        new boost::asio::ip::tcp::acceptor(the_io_service, *endpoint));
    return *this;
  }

  server& threads(size_t n) {
    num_threads = n;
    return *this;
  }

  void run() {
    resources.clear();
    for (auto it = route.begin(); it != route.end(); it++) {
      std::cout << "route " << it->first << std::endl;
      resources.push_back(it);
    }
    accept();

    workers.clear();
    for (size_t c = 1; c < num_threads; c++) {
      workers.emplace_back([this]() { the_io_service.run(); });
    }

    std::cout << "server runing on 0.0.0.0:" << port_num << " ..." << std::endl;
    the_io_service.run();

    for (auto& t : workers) {
      t.join();
    }
  }

  void stop() { the_io_service.stop(); }

  void accept() {
    std::shared_ptr<socket> sock(new socket(the_io_service));

    acceptor->async_accept(*sock,
                           [this, sock](const boost::system::error_code& ec) {
      // Immediately start accepting a new connection
      accept();

      if (!ec) {
        do_read(sock);
      }
    });
  }

  void do_read(std::shared_ptr<socket> sock) {
    std::shared_ptr<request> req(new request());

    std::shared_ptr<boost::asio::deadline_timer> timer;
    if (timeout_req > 0) timer = set_timeout(sock, timeout_req);

    boost::asio::async_read_until(
        *sock, req->buf, "\r\n\r\n",
        [this, sock, req, timer](const boost::system::error_code& ec,
                                 size_t sent) {
          if (timeout_req > 0) timer->cancel();
          if (!ec) {
            size_t remained = req->buf.size() - sent;

            parse(req, req->content);

            if (req->headers.count("Content-Length") > 0) {
              std::shared_ptr<boost::asio::deadline_timer> timer;
              if (timeout_content > 0)
                timer = set_timeout(sock, timeout_content);

              boost::asio::async_read(
                  *sock, req->buf,
                  boost::asio::transfer_exactly(
                      stoull(req->headers["Content-Length"]) - remained),
                  [this, sock, req, timer](const boost::system::error_code& ec,
                                           size_t sent) {
                    if (timeout_content > 0) timer->cancel();
                    if (!ec) do_write(sock, req);
                  });
            } else {
              do_write(sock, req);
            }
          }
        });
  }

  void do_write(std::shared_ptr<socket> sock, std::shared_ptr<request> req) {
    response res = handle(req);
    std::shared_ptr<boost::asio::streambuf> write_buffer(
        new boost::asio::streambuf);
    std::ostream res_stream(write_buffer.get());
    res_stream << res.commit();

    std::shared_ptr<boost::asio::deadline_timer> timer;
    if (timeout_content > 0) timer = set_timeout(sock, timeout_content);

    boost::asio::async_write(
        *sock, *write_buffer,
        [this, sock, req, write_buffer, timer](
            const boost::system::error_code& ec, size_t sent) {
          if (timeout_content > 0) timer->cancel();
          if (!ec && stof(req->http_version) > 1.05) do_read(sock);
        });
    return;
  }

  response handle(std::shared_ptr<request> req) {
    for (auto r : resources) {
      std::regex e(r->first);
      std::smatch sm_res;
      if (std::regex_match(req->path, sm_res, e)) {
        if (r->second.count(req->method) > 0) {
          req->path_match = move(sm_res);
          return r->second[req->method](req).commit();
        }
      }
    }

    // router not found
    return response(404);
  }

  void parse(std::shared_ptr<request> req, std::istream& is) const {
    std::regex e("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    std::smatch sm;
    std::string line;
    getline(is, line);
    line.pop_back();
    if (std::regex_match(line, sm, e)) {
      req->method = sm[1];
      std::cout << "method:" << req->method << std::endl;
      std::string path = sm[2];
      if (path.find('?') != std::string::npos) {
        boost::char_separator<char> seps("?&");
        boost::tokenizer<boost::char_separator<char>> tokens(path, seps);
        auto itr = tokens.begin();
        req->path = *itr;
        ++itr;
        for (; itr != tokens.end(); ++itr) {
          std::cout << "param:" << *itr << std::endl;
          req->params.push_back(*itr);
        }
      }
      std::cout << "path:" << req->path << std::endl;
      req->http_version = sm[3];

      bool matched{false};
      e = "^([^:]*): ?(.*)$";
      do {
        getline(is, line);
        line.pop_back();
        matched = std::regex_match(line, sm, e);
        if (matched) {
          req->headers[sm[1]] = sm[2];
        }

      } while (matched == true);
    }
  }

  std::shared_ptr<boost::asio::deadline_timer> set_timeout(
      std::shared_ptr<socket> sock, size_t seconds) {
    std::shared_ptr<boost::asio::deadline_timer> timer(
        new boost::asio::deadline_timer(the_io_service));
    timer->expires_from_now(boost::posix_time::seconds(seconds));
    timer->async_wait([sock](const boost::system::error_code& ec) {
      if (!ec) {
        sock->lowest_layer().shutdown(
            boost::asio::ip::tcp::socket::shutdown_both);
        sock->lowest_layer().close();
      }
    });
    return timer;
  }
};
}  // namespace http
