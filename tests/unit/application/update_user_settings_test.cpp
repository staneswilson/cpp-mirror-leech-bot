// ---------------------------------------------------------------------------
// update_user_settings_test.cpp - unit tests for UpdateUserSettings.
// ---------------------------------------------------------------------------

#include "in_memory_user_settings_repository.hpp"

#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/application/update_user_settings.hpp>

namespace asio = boost::asio;

using cmlb::application::UpdateUserSettings;
using cmlb::application::UpdateUserSettingsRequest;
using cmlb::core::ErrorCode;
using cmlb::domain::UploadDestination;
using cmlb::domain::UserId;
using cmlb::infrastructure::persistence::UserSettingsRecord;
using cmlb::test_support::InMemoryUserSettingsRepository;

namespace {

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

} // namespace

TEST_CASE("UpdateUserSettings creates a record on first use", "[application][user_settings]") {
    InMemoryUserSettingsRepository repo;
    UpdateUserSettings uc{repo};

    asio::io_context ctx;
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<UserSettingsRecord>> {
        co_return co_await uc.execute(
            UpdateUserSettingsRequest{UserId{42}, [](UserSettingsRecord& r) {
                                          r.upload_as_document = true;
                                          r.rclone_remote = "gdrive:Backups";
                                      }});
    });

    REQUIRE(result.has_value());
    CHECK(result->user_id == UserId{42});
    CHECK(result->upload_as_document == true);
    REQUIRE(result->rclone_remote.has_value());
    CHECK(*result->rclone_remote == "gdrive:Backups");
    CHECK(repo.size() == 1);
}

TEST_CASE("UpdateUserSettings updates an existing record in place",
          "[application][user_settings]") {
    InMemoryUserSettingsRepository repo;
    UpdateUserSettings uc{repo};

    asio::io_context ctx;
    (void)run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<UserSettingsRecord>> {
        co_return co_await uc.execute(
            UpdateUserSettingsRequest{UserId{7}, [](UserSettingsRecord& r) {
                                          r.mirror_destination = UploadDestination::Rclone;
                                      }});
    });

    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<UserSettingsRecord>> {
        co_return co_await uc.execute(
            UpdateUserSettingsRequest{UserId{7}, [](UserSettingsRecord& r) {
                                          r.gdrive_folder_id = "FOLDER123";
                                      }});
    });

    REQUIRE(result.has_value());
    CHECK(result->mirror_destination == UploadDestination::Rclone);
    REQUIRE(result->gdrive_folder_id.has_value());
    CHECK(*result->gdrive_folder_id == "FOLDER123");
    CHECK(repo.size() == 1);
}

TEST_CASE("UpdateUserSettings rejects null mutators", "[application][user_settings]") {
    InMemoryUserSettingsRepository repo;
    UpdateUserSettings uc{repo};

    asio::io_context ctx;
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<UserSettingsRecord>> {
        co_return co_await uc.execute(UpdateUserSettingsRequest{UserId{1}, {}});
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
}
