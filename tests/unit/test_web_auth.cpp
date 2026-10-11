/**
 * @file tests/unit/test_web_auth.cpp
 * @brief Web UI login sessions: issue, refresh rotation, expiry, revocation, persistence, cookies, CSRF origins.
 */
#include "../tests_common.h"

#include "src/web_auth.h"

#include <filesystem>
#include <fstream>

namespace {
  using namespace std::chrono_literals;
  using web_auth::clock_t;

  struct fixture_t {
    clock_t::time_point now = clock_t::time_point {std::chrono::seconds(1'800'000'000)};
    int counter = 0;
    std::filesystem::path file;

    fixture_t() {
      file = std::filesystem::temp_directory_path() / ("lavapollo_auth_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                                                       ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
      std::filesystem::remove(file);
    }

    ~fixture_t() {
      std::filesystem::remove(file);
      std::filesystem::remove(std::filesystem::path {file} += ".tmp");
    }

    web_auth::dependencies_t deps() {
      return {
        [this] {
          return now;
        },
        [this](std::size_t) {
          return "tok" + std::to_string(++counter);
        },
        [](const std::string &value) {
          return "hash:" + value;
        },
      };
    }

    web_auth::session_store_t store() {
      return web_auth::session_store_t {file, deps()};
    }
  };

  constexpr auto session_ttl = std::chrono::seconds(2h);
}  // namespace

TEST(WebAuthTest, IssuedSessionValidatesUntilItsSessionTtl) {
  fixture_t f;
  auto store = f.store();
  const auto tokens = store.issue("admin", false, "Firefox/1 (Windows)", "192.168.1.2", session_ttl, web_auth::default_refresh_ttl);

  EXPECT_EQ(tokens.session_ttl, session_ttl);
  EXPECT_EQ(tokens.refresh_ttl, web_auth::default_refresh_ttl);
  EXPECT_TRUE(store.validate(tokens.session_token).has_value());
  EXPECT_FALSE(store.validate(tokens.refresh_token).has_value()) << "a refresh token is not a session token";
  EXPECT_FALSE(store.validate("").has_value());

  f.now += session_ttl + 1s;
  EXPECT_FALSE(store.validate(tokens.session_token).has_value());
}

TEST(WebAuthTest, RefreshRotatesBothTokensAndKeepsTheId) {
  fixture_t f;
  auto store = f.store();
  const auto first = store.issue("admin", true, "", "", session_ttl, 7 * 24h);
  const auto id = store.validate(first.session_token);
  ASSERT_TRUE(id);

  f.now += session_ttl + 1s;
  const auto second = store.refresh(first.refresh_token, "", "", session_ttl);
  ASSERT_TRUE(second);
  EXPECT_NE(second->session_token, first.session_token);
  EXPECT_NE(second->refresh_token, first.refresh_token);
  EXPECT_EQ(store.validate(second->session_token), id) << "the login keeps its id across refreshes";

  EXPECT_FALSE(store.refresh(first.refresh_token, "", "", session_ttl)) << "a used refresh token is spent";
  EXPECT_FALSE(store.validate(first.session_token));
}

TEST(WebAuthTest, RefreshLifetimeDoesNotSlide) {
  fixture_t f;
  auto store = f.store();
  auto tokens = store.issue("admin", true, "", "", session_ttl, 24h);

  // Refreshing every two hours never pushes the end of the login past its first 24 hours
  for (int i = 0; i < 11; i++) {
    f.now += session_ttl;
    auto next = store.refresh(tokens.refresh_token, "", "", session_ttl);
    ASSERT_TRUE(next) << "refresh " << i;
    tokens = *next;
  }
  EXPECT_EQ(tokens.refresh_ttl, 2h);

  f.now += session_ttl + 1s;
  EXPECT_FALSE(store.refresh(tokens.refresh_token, "", "", session_ttl));
  EXPECT_TRUE(store.list().empty());
}

TEST(WebAuthTest, RefreshLifetimeIsNeverShorterThanTheSession) {
  fixture_t f;
  auto store = f.store();
  const auto tokens = store.issue("admin", false, "", "", 48h, 24h);
  EXPECT_EQ(tokens.refresh_ttl, 48h) << "a refresh token never runs out before its session token";
  EXPECT_EQ(tokens.session_ttl, 48h);
}

TEST(WebAuthTest, RevocationEndsOneLoginOrAll) {
  fixture_t f;
  auto store = f.store();
  const auto a = store.issue("admin", false, "", "", session_ttl, 24h);
  const auto b = store.issue("admin", false, "", "", session_ttl, 24h);
  const auto c = store.issue("admin", false, "", "", session_ttl, 24h);

  const auto a_id = store.validate(a.session_token);
  ASSERT_TRUE(a_id);
  EXPECT_TRUE(store.revoke(*a_id));
  EXPECT_FALSE(store.revoke(*a_id));
  EXPECT_FALSE(store.validate(a.session_token));
  EXPECT_FALSE(store.refresh(a.refresh_token, "", "", session_ttl));

  store.revoke_tokens("", b.refresh_token);
  EXPECT_FALSE(store.validate(b.session_token)) << "logout by refresh token ends the whole login";
  EXPECT_TRUE(store.validate(c.session_token));

  store.revoke_all();
  EXPECT_FALSE(store.validate(c.session_token));
  EXPECT_TRUE(store.list().empty());
}

TEST(WebAuthTest, LoginsSurviveARestartAndExpiredOnesDoNot) {
  fixture_t f;
  web_auth::tokens_t kept;
  {
    auto store = f.store();
    kept = store.issue("admin", true, "Mozilla/5.0 (Android 14) Chrome/120", "192.168.1.3", session_ttl, 7 * 24h);
    store.issue("admin", false, "", "", session_ttl, 24h);
  }

  f.now += 25h;
  auto restarted = f.store();
  restarted.load();
  const auto sessions = restarted.list();
  ASSERT_EQ(sessions.size(), 1u) << "the 24 h login expired while the host was down";
  EXPECT_EQ(sessions.front().device, "Chrome on Android");
  EXPECT_EQ(sessions.front().address, "192.168.1.3");
  EXPECT_TRUE(sessions.front().remember_me);
  EXPECT_TRUE(restarted.refresh(kept.refresh_token, "", "", session_ttl));

  std::ifstream in {f.file};
  const std::string contents {std::istreambuf_iterator<char> {in}, {}};
  EXPECT_EQ(contents.find("\"" + kept.refresh_token + "\""), std::string::npos) << "only token hashes are stored";
}

TEST(WebAuthTest, UnreadableFileMeansNoLogins) {
  fixture_t f;
  {
    std::ofstream out {f.file};
    out << "{ not json";
  }
  auto store = f.store();
  store.load();
  EXPECT_TRUE(store.list().empty());
}

TEST(WebAuthTest, ListFlagsTheCurrentLogin) {
  fixture_t f;
  auto store = f.store();
  const auto a = store.issue("admin", false, "", "", session_ttl, 24h);
  const auto a_id = store.validate(a.session_token);
  f.now += 10min;
  store.issue("admin", false, "", "", session_ttl, 24h);

  const auto sessions = store.list(*a_id);
  ASSERT_EQ(sessions.size(), 2u);
  EXPECT_FALSE(sessions[0].current) << "most recently seen first";
  EXPECT_TRUE(sessions[1].current);
}

TEST(WebAuthTest, CookieHelpers) {
  EXPECT_EQ(web_auth::cookie_value("a=1; __Host-apollo_session=abc; __Host-apollo_refresh=def", web_auth::session_cookie_name), "abc");
  EXPECT_EQ(web_auth::cookie_value("x__Host-apollo_session=evil; __Host-apollo_refresh=def", web_auth::session_cookie_name), "")
    << "a name is matched whole, not as a suffix";
  EXPECT_EQ(web_auth::cookie_value("", "auth"), "");

  EXPECT_EQ(web_auth::set_cookie("__Host-x", "v", std::chrono::seconds(60)), "__Host-x=v; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=60");
  EXPECT_EQ(web_auth::set_cookie("__Host-x", "v", std::nullopt), "__Host-x=v; Path=/; Secure; HttpOnly; SameSite=Strict");
  EXPECT_EQ(web_auth::clear_cookie("__Host-x"), "__Host-x=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
}

TEST(WebAuthTest, OriginCheckAcceptsTheUisOwnAddress) {
  const std::vector<std::string> none;
  EXPECT_TRUE(web_auth::origin_allowed("https://192.168.1.10:47990", "192.168.1.10:47990", none));
  EXPECT_TRUE(web_auth::origin_allowed("https://192.168.1.10:47990/config", "192.168.1.10:47990", none)) << "a Referer";
  EXPECT_TRUE(web_auth::origin_allowed("https://PC.local:47990", "pc.local:47990", none));

  EXPECT_FALSE(web_auth::origin_allowed("https://evil.example", "192.168.1.10:47990", none));
  EXPECT_FALSE(web_auth::origin_allowed("https://192.168.1.10:8443", "192.168.1.10:47990", none)) << "another port is another origin";
  EXPECT_FALSE(web_auth::origin_allowed("http://192.168.1.10:47990", "192.168.1.10:47990", none)) << "the UI is HTTPS only";
  EXPECT_FALSE(web_auth::origin_allowed("null", "192.168.1.10:47990", none));

  const std::vector<std::string> proxy {"https://apollo.example.com"};
  EXPECT_TRUE(web_auth::origin_allowed("https://apollo.example.com", "10.0.0.2:47990", proxy));
  EXPECT_TRUE(web_auth::origin_allowed("https://apollo.example.com:8443", "10.0.0.2:47990", proxy));
  EXPECT_FALSE(web_auth::origin_allowed("https://apollo.example.com.evil.example", "10.0.0.2:47990", proxy));
}

TEST(WebAuthTest, DeviceLabels) {
  EXPECT_EQ(web_auth::device_label("Mozilla/5.0 (Windows NT 10.0; Win64; x64) Gecko/20100101 Firefox/128.0", ""), "Firefox on Windows");
  EXPECT_EQ(web_auth::device_label("Mozilla/5.0 (Windows NT 10.0) AppleWebKit/537.36 Chrome/120 Safari/537.36 Edg/120", ""), "Edge on Windows");
  EXPECT_EQ(web_auth::device_label("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) Version/17.0 Safari/604.1", ""), "Safari on iOS");
  EXPECT_EQ(web_auth::device_label("", "192.168.1.4"), "192.168.1.4");
  EXPECT_EQ(web_auth::device_label("", ""), "Unknown device");
}
