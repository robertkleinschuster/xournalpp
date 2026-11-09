/*
 * Xournal++
 *
 * Device identifier
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */
#pragma once

#include <gdk/gdk.h>

struct DeviceId {
    constexpr DeviceId() = default;
    explicit DeviceId(const GdkDevice* id);
    void reset(const GdkDevice* id = nullptr);
    explicit operator bool() const;
    bool operator==(const DeviceId& o) const;
    bool operator!=(const DeviceId& o) const;
    /// Fork: a mouse, touchpad or trackpoint (not a pen or touchscreen)
    bool isPointer() const { return pointer; }

private:
    const void* id = nullptr;
    bool trackpointOrTouchpad = false;
    bool pointer = false;
};
