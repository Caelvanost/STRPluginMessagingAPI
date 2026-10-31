#pragma once

#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <type_traits>
#include <vector>

namespace STRPMInventoryNative
{
    // ABI-compatible subset of TiltedCore 0.2.9 Allocator/StlAllocator used by
    // STR 1.8.4's TiltedPhoques::Vector. This lets STR's own
    // TESObjectREFR::GetItemFromExtraData populate temporary enchant effects
    // without linking STRPM against private TiltedCore symbols.
    struct Allocator
    {
        virtual ~Allocator() = default;

        virtual void* Allocate(std::size_t size) noexcept
        {
            return std::malloc(size);
        }

        virtual void Free(void* data) noexcept
        {
            std::free(data);
        }

        virtual std::size_t Size(void* data) noexcept
        {
#if defined(_WIN32)
            return data ? _msize(data) : 0;
#else
            (void)data;
            return 0;
#endif
        }
    };

    inline Allocator& GetAllocator() noexcept
    {
        static Allocator allocator;
        return allocator;
    }

    template <class T>
    struct StlAllocator
    {
        using value_type = T;

        StlAllocator() noexcept
            : allocator(&GetAllocator())
        {
        }

        template <class U>
        StlAllocator(const StlAllocator<U>& rhs) noexcept
            : allocator(rhs.allocator)
        {
        }

        [[nodiscard]] T* allocate(std::size_t count)
        {
            if (count > static_cast<std::size_t>(-1) / sizeof(T))
                throw std::bad_alloc();

            if (auto* data =
                    static_cast<T*>(allocator->Allocate(count * sizeof(T))))
            {
                return data;
            }
            throw std::bad_alloc();
        }

        void deallocate(T* data, std::size_t) noexcept
        {
            allocator->Free(data);
        }

        template <class>
        friend struct StlAllocator;

        Allocator* allocator{ nullptr };
    };

    template <class T, class U>
    bool operator==(const StlAllocator<T>& left, const StlAllocator<U>& right) noexcept
    {
        return left.allocator == right.allocator;
    }

    template <class T, class U>
    bool operator!=(const StlAllocator<T>& left, const StlAllocator<U>& right) noexcept
    {
        return !(left == right);
    }

    template <class T>
    using Vector = std::vector<T, StlAllocator<T>>;

    struct GameId
    {
        std::uint32_t BaseId{ 0 };
        std::uint32_t ModId{ 0 };
    };

    struct EffectItem
    {
        float Magnitude{ 0.0f };
        std::int32_t Area{ 0 };
        std::int32_t Duration{ 0 };
        float RawCost{ 0.0f };
        GameId EffectId{};
    };

    struct EnchantmentData
    {
        bool IsWeapon{ false };
        Vector<EffectItem> Effects{};
    };

    struct Entry
    {
        GameId BaseId{};
        std::int32_t Count{ 0 };

        float ExtraCharge{ 0.0f };

        GameId ExtraEnchantId{};
        std::uint16_t ExtraEnchantCharge{ 0 };
        EnchantmentData EnchantData{};

        float ExtraHealth{ 0.0f };

        GameId ExtraPoisonId{};
        std::uint32_t ExtraPoisonCount{ 0 };

        std::int32_t ExtraSoulLevel{ 0 };

        bool ExtraEnchantRemoveUnequip{ false };
        bool ExtraWorn{ false };
        bool ExtraWornLeft{ false };
        bool IsQuestItem{ false };
    };

#if defined(_WIN64)
    static_assert(sizeof(StlAllocator<std::uint8_t>) == 8);
    static_assert(sizeof(Vector<EffectItem>) == 32);
    static_assert(sizeof(GameId) == 8);
    static_assert(sizeof(EffectItem) == 24);
    static_assert(sizeof(EnchantmentData) == 40);
    static_assert(offsetof(Entry, BaseId) == 0);
    static_assert(offsetof(Entry, Count) == 8);
    static_assert(offsetof(Entry, ExtraCharge) == 12);
    static_assert(offsetof(Entry, ExtraEnchantId) == 16);
    static_assert(offsetof(Entry, ExtraEnchantCharge) == 24);
    static_assert(offsetof(Entry, EnchantData) == 32);
    static_assert(offsetof(Entry, ExtraHealth) == 72);
    static_assert(offsetof(Entry, ExtraPoisonId) == 76);
    static_assert(offsetof(Entry, ExtraPoisonCount) == 84);
    static_assert(offsetof(Entry, ExtraSoulLevel) == 88);
    static_assert(offsetof(Entry, ExtraEnchantRemoveUnequip) == 92);
    static_assert(sizeof(Entry) == 96);
#endif

    inline STRPM::InventoryGameId ToPublic(const GameId& value) noexcept
    {
        return { value.BaseId, value.ModId };
    }

    inline GameId ToNative(const STRPM::InventoryGameId& value) noexcept
    {
        return { value.baseId, value.modId };
    }

    inline STRPM::Result ToPublic(
        const Entry& source,
        STRPM::InventoryEntry& destination) noexcept
    {
        destination = {};
        destination.baseId = ToPublic(source.BaseId);
        destination.count = source.Count;
        destination.extraCharge = source.ExtraCharge;
        destination.extraEnchantId = ToPublic(source.ExtraEnchantId);
        destination.extraEnchantCharge = source.ExtraEnchantCharge;
        destination.extraHealth = source.ExtraHealth;
        destination.extraPoisonId = ToPublic(source.ExtraPoisonId);
        destination.extraPoisonCount = source.ExtraPoisonCount;
        destination.extraSoulLevel = source.ExtraSoulLevel;

        if (source.EnchantData.IsWeapon)
            destination.flags |= STRPM::kInventoryEntryEnchantIsWeapon;
        if (source.ExtraEnchantRemoveUnequip)
            destination.flags |= STRPM::kInventoryEntryEnchantRemoveOnUnequip;
        if (source.ExtraWorn)
            destination.flags |= STRPM::kInventoryEntryWorn;
        if (source.ExtraWornLeft)
            destination.flags |= STRPM::kInventoryEntryWornLeft;
        if (source.IsQuestItem)
            destination.flags |= STRPM::kInventoryEntryQuestItem;

        if (source.EnchantData.Effects.size() > STRPM::kMaxInventoryEffects)
            return STRPM::Result::kUnsupportedItem;

        destination.enchantEffectCount =
            static_cast<std::uint16_t>(source.EnchantData.Effects.size());
        for (std::size_t i = 0; i < source.EnchantData.Effects.size(); ++i)
        {
            const auto& input = source.EnchantData.Effects[i];
            auto& output = destination.enchantEffects[i];
            output.magnitude = input.Magnitude;
            output.area = input.Area;
            output.duration = input.Duration;
            output.rawCost = input.RawCost;
            output.effectId = ToPublic(input.EffectId);
        }

        return STRPM::Result::kOk;
    }

    inline STRPM::Result ToNative(
        const STRPM::InventoryEntry& source,
        Entry& destination)
    {
        if (source.enchantEffectCount > STRPM::kMaxInventoryEffects)
            return STRPM::Result::kUnsupportedItem;

        destination = {};
        destination.BaseId = ToNative(source.baseId);
        destination.Count = source.count;
        destination.ExtraCharge = source.extraCharge;
        destination.ExtraEnchantId = ToNative(source.extraEnchantId);
        destination.ExtraEnchantCharge = source.extraEnchantCharge;
        destination.ExtraHealth = source.extraHealth;
        destination.ExtraPoisonId = ToNative(source.extraPoisonId);
        destination.ExtraPoisonCount = source.extraPoisonCount;
        destination.ExtraSoulLevel = source.extraSoulLevel;
        destination.EnchantData.IsWeapon =
            (source.flags & STRPM::kInventoryEntryEnchantIsWeapon) != 0;
        destination.ExtraEnchantRemoveUnequip =
            (source.flags & STRPM::kInventoryEntryEnchantRemoveOnUnequip) != 0;
        destination.ExtraWorn =
            (source.flags & STRPM::kInventoryEntryWorn) != 0;
        destination.ExtraWornLeft =
            (source.flags & STRPM::kInventoryEntryWornLeft) != 0;
        destination.IsQuestItem =
            (source.flags & STRPM::kInventoryEntryQuestItem) != 0;

        destination.EnchantData.Effects.reserve(source.enchantEffectCount);
        for (std::size_t i = 0; i < source.enchantEffectCount; ++i)
        {
            const auto& input = source.enchantEffects[i];
            EffectItem output{};
            output.Magnitude = input.magnitude;
            output.Area = input.area;
            output.Duration = input.duration;
            output.RawCost = input.rawCost;
            output.EffectId = ToNative(input.effectId);
            destination.EnchantData.Effects.push_back(output);
        }

        return STRPM::Result::kOk;
    }
}
