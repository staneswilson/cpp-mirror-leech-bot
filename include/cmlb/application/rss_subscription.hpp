#pragma once

#include <cstdint>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

/// @file rss_subscription.hpp
/// @brief Aggregated RSS subscription use case (add / list / remove).

namespace cmlb::application {

/// Bundled use case exposing the three RSS subscription operations.
///
/// `remove` enforces that the requester is allowed to delete the feed: the
/// requester's @p requester_chat must match the feed's `chat` field. The
/// @p requester user id is logged for audit but the chat is the authoritative
/// ownership key (the persistence schema does not store a per-user owner).
class RssSubscription {
public:
    RssSubscription(cmlb::infrastructure::persistence::RssFeedRepository& repo,
                    cmlb::infrastructure::telegram::MessengerInterface& messenger) noexcept;

    /// Inserts a new feed. The returned id is the SQLite-assigned primary key.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::int64_t>> add(
        cmlb::infrastructure::persistence::RssFeed feed);

    /// Returns every enabled feed whose `chat` matches @p chat.
    [[nodiscard]] boost::asio::awaitable<
        cmlb::core::Result<std::vector<cmlb::infrastructure::persistence::RssFeed>>>
    list_for_chat(cmlb::domain::ChatId chat);

    /// Removes the feed identified by @p feed_id. Returns `PermissionDenied`
    /// if the feed's chat differs from @p requester_chat, or `NotFound` if
    /// no feed matches @p feed_id.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> remove(
        std::int64_t feed_id, cmlb::domain::UserId requester, cmlb::domain::ChatId requester_chat);

private:
    cmlb::infrastructure::persistence::RssFeedRepository& repo_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
};

} // namespace cmlb::application
