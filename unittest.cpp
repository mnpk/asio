#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include "server.hpp"

TEST_CASE("response creation", "[response]") {
  http::response r;

  REQUIRE(r.code == 200);
  REQUIRE(r.content.empty());
  REQUIRE(r.headers.empty());
}

TEST_CASE("response creation with non-error status code", "[response]") {
  http::response r(302);

  REQUIRE(r.code == 302);
  REQUIRE(r.content.empty());
  REQUIRE(r.headers.empty());

  SECTION("headers will be filled after commit()", "[response]") {
    r.commit();
    REQUIRE(r.code == 302);
    REQUIRE(r.content.empty());
    REQUIRE(r.headers.empty());
  }
}

TEST_CASE("response creation with error status code", "[response]") {
  http::response r(400);

  REQUIRE(r.code == 400);
  REQUIRE(r.content.empty());
  REQUIRE(r.headers.empty());

  SECTION("content and headers will be filled after commit()", "[response]") {
    r.commit();
    REQUIRE(r.code == 400);
    REQUIRE(r.content == "400 Bad Request\r\n");
    REQUIRE(r.headers.count("content-length") > 0);
  }
}

TEST_CASE("response creation with text content", "[response]") {
  http::response r("hello");

  REQUIRE(r.code == 200);
  REQUIRE(r.content == "hello");
  REQUIRE(r.headers.empty());

  SECTION("headers will be filled after commit()", "[response]") {
    r.commit();
    REQUIRE(r.code == 200);
    REQUIRE(r.content == "hello");
    REQUIRE(r.headers.count("content-length") > 0);
  }
}

TEST_CASE("response creation with json content", "[response]") {
  http::json j;
  j["message"] = "hello";
  http::response r(j);

  REQUIRE(r.code == 200);
  REQUIRE(r.content == j.dump());
  REQUIRE(r.headers.count("content-length") == 0);
  REQUIRE(r.headers.count("content-type") > 0);

  SECTION("headers will be filled after commit()", "[response]") {
    r.commit();
    REQUIRE(r.code == 200);
    REQUIRE(r.content == j.dump());
    REQUIRE(r.headers.count("content-length") > 0);
    REQUIRE(r.headers.count("content-type") > 0);
  }
}
