#pragma once

#include <span>
#include <string_view>
#include <vector>

#include <cmlb/domain/identifiers.hpp>

/// @file authority.hpp
/// @brief Permission model + Authority service for command authorisation.

namespace cmlb::domain {

/// Hierarchical permission level. Implicitly ordered:
/// Anyone (0) < User (1) < Admin (2) < Owner (3). Higher levels include all
/// privileges of lower levels (see @ref Authority::can_run).
enum class Permission {
    Anyone = 0,
    User   = 1,
    Admin  = 2,
    Owner  = 3,
};

[[nodiscard]] std::string_view to_string(Permission permission) noexcept;

/// Parses a permission name. Unknown strings yield @ref Permission::Anyone
/// (the safest, least-privileged default).
[[nodiscard]] Permission from_string(std::string_view input) noexcept;

/// Pure-logic authorisation service. Holds the configured access lists and
/// answers two questions: what permission level a (user, chat) pair has, and
/// whether that level is sufficient to run a command requiring level R.
class Authority {
public:
    Authority(UserId owner,
              std::span<const UserId> sudo_users,
              std::span<const ChatId> authorized_chats);

    [[nodiscard]] Permission of(UserId user, ChatId chat) const noexcept;

    [[nodiscard]] bool can_run(UserId user, ChatId chat, Permission required) const noexcept;

private:
    UserId owner_;
    std::vector<UserId> sudo_users_;
    std::vector<ChatId> authorized_chats_;
};

}  // namespace cmlb::domain
