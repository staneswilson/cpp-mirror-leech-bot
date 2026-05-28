#include <cmlb/domain/authority.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace cmlb::domain {

std::string_view to_string(Permission permission) noexcept {
    switch (permission) {
        case Permission::Anyone: return "anyone";
        case Permission::User:   return "user";
        case Permission::Admin:  return "admin";
        case Permission::Owner:  return "owner";
    }
    return "anyone";
}

Permission from_string(std::string_view input) noexcept {
    std::string lower;
    lower.reserve(input.size());
    for (const char ch : input) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "owner") return Permission::Owner;
    if (lower == "admin" || lower == "sudo") return Permission::Admin;
    if (lower == "user") return Permission::User;
    return Permission::Anyone;
}

Authority::Authority(UserId owner,
                     std::span<const UserId> sudo_users,
                     std::span<const ChatId> authorized_chats)
    : owner_{std::move(owner)},
      sudo_users_{sudo_users.begin(), sudo_users.end()},
      authorized_chats_{authorized_chats.begin(), authorized_chats.end()} {}

Permission Authority::of(UserId user, ChatId chat) const noexcept {
    if (user == owner_) {
        return Permission::Owner;
    }
    if (std::ranges::find(sudo_users_, user) != sudo_users_.end()) {
        return Permission::Admin;
    }
    if (std::ranges::find(authorized_chats_, chat) != authorized_chats_.end()) {
        return Permission::User;
    }
    return Permission::Anyone;
}

bool Authority::can_run(UserId user, ChatId chat, Permission required) const noexcept {
    return static_cast<int>(of(user, chat)) >= static_cast<int>(required);
}

}  // namespace cmlb::domain
