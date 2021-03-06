// Concord
//
// Copyright (c) 2020-2021 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the
// "License").  You may not use this product except in compliance with the
// Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#pragma once

#include "assertUtils.hpp"
#include "kv_types.hpp"
#include "categorized_kvbc_msgs.cmf.hpp"

#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>

// Categorized key-value updates for KVBC blocks. Every category supports different properties and functionalities.
// Note1: Empty category IDs are invalid and not supported.
// Note2: Using the same category ID for different category types is an error.
namespace concord::kvbc::categorization {

// Keys in immutable categories have a single version only and can be tagged. Updating keys in immutable categories is
// undefined behavior. Persists key-values directly in the underlying key-value store. All key-values become stale since
// the block they are being added in and this cannot be turned off. Explicit deletes are not supported. Supports an
// option to calculate a root hash per tag from the key-values in the update. The root hash can be used for key proofs
// per tag.
struct ImmutableUpdates {
  ImmutableUpdates() = default;
  ImmutableUpdates(ImmutableUpdates&& other) = default;
  ImmutableUpdates& operator=(ImmutableUpdates&& other) = default;

  // Do not allow copy
  ImmutableUpdates(ImmutableUpdates& other) = delete;
  ImmutableUpdates& operator=(ImmutableUpdates& other) = delete;

  struct ImmutableValue {
    ImmutableValue(std::string&& val, std::set<std::string>&& tags) {
      update_.data = std::move(val);
      for (auto it = tags.begin(); it != tags.end();) {
        // Save the iterator as extract() invalidates it.
        auto extracted_it = it;
        ++it;
        update_.tags.emplace_back(tags.extract(extracted_it).value());
      }
    }

   private:
    ImmutableValueUpdate update_;
    friend struct ImmutableUpdates;
  };

  void addUpdate(std::string&& key, ImmutableValue&& val, bool allow_update = false) {
    data_.kv[std::move(key)] = std::move(val.update_);
  }

  void calculateRootHash(const bool hash) { data_.calculate_root_hash = hash; }

  size_t size() const { return data_.kv.size(); }

  const ImmutableInput& getData() const { return data_; }

 private:
  ImmutableInput data_;
  friend struct Updates;
};

// Updates for a versioned key-value category.
// Persists versioned (by block ID) key-values directly in the underlying key-value store.
// Supports an option to calculate a root hash from the key-values in the update. The root hash can be used for key
// proofs.
struct VersionedUpdates {
  VersionedUpdates() = default;
  VersionedUpdates(VersionedUpdates&& other) = default;
  VersionedUpdates& operator=(VersionedUpdates&& other) = default;

  // Do not allow copy
  VersionedUpdates(const VersionedUpdates& other) = delete;
  VersionedUpdates& operator=(VersionedUpdates& other) = delete;
  struct Value {
    std::string data;

    // Mark the key-value stale during the update itself.
    bool stale_on_update{false};
  };

  void addUpdate(std::string&& key, Value&& val, bool allow_update = false) {
    data_.kv[std::move(key)] = ValueWithFlags{std::move(val.data), val.stale_on_update};
  }

  // Set a value with no flags set
  void addUpdate(std::string&& key, std::string&& val, bool allow_update = false) {
    data_.kv[std::move(key)] = ValueWithFlags{std::move(val), false};
  }

  void addDelete(std::string&& key) {
    if (const auto [itr, inserted] = unique_deletes_.insert(key); !inserted) {
      *itr;  // disable unused variable
      // Log warn
      return;
    }
    data_.deletes.emplace_back(std::move(key));
  }

  void calculateRootHash(const bool hash) { data_.calculate_root_hash = hash; }

  std::size_t size() const { return data_.kv.size(); }

  const VersionedInput& getData() const { return data_; }

 private:
  VersionedInput data_;
  std::set<std::string> unique_deletes_;
  friend struct Updates;
};

// Updates for a merkle tree category.
// Persists key-values in a merkle tree that is constructed on top of the underlying key-value store.
struct BlockMerkleUpdates {
  BlockMerkleUpdates() = default;
  BlockMerkleUpdates(BlockMerkleUpdates&& other) = default;
  BlockMerkleUpdates(BlockMerkleInput&& data) : data_{std::move(data)} {}
  BlockMerkleUpdates& operator=(BlockMerkleUpdates&& other) = default;

  // Do not allow copy
  BlockMerkleUpdates(BlockMerkleUpdates& other) = delete;
  BlockMerkleUpdates& operator=(BlockMerkleUpdates& other) = delete;

  void addUpdate(std::string&& key, std::string&& val, bool allow_update = false) {
    data_.kv[std::move(key)] = std::move(val);
  }

  void addDelete(std::string&& key) {
    if (const auto [itr, inserted] = unique_deletes_.insert(key); !inserted) {
      *itr;  // disable unused variable
      // Log warn
      return;
    }
    data_.deletes.emplace_back(std::move(key));
  }

  const BlockMerkleInput& getData() const { return data_; }

  std::size_t size() const { return data_.kv.size(); }

 private:
  BlockMerkleInput data_;
  std::set<std::string> unique_deletes_;
  friend struct Updates;
};

// Updates contains a list of updates for different categories.
struct Updates {
  Updates() = default;
  Updates(CategoryInput&& updates) : category_updates_{std::move(updates)} {}
  bool operator==(const Updates& other) { return category_updates_ == other.category_updates_; }
  void add(const std::string& category_id, BlockMerkleUpdates&& updates) {
    block_merkle_size += updates.size();
    if (const auto [itr, inserted] = category_updates_.kv.try_emplace(category_id, std::move(updates.data_));
        !inserted) {
      (void)itr;  // disable unused variable
      throw std::logic_error{std::string("Only one update for category is allowed. type: BlockMerkle, category: ") +
                             category_id};
    }
  }

  void add(const std::string& category_id, VersionedUpdates&& updates) {
    versioned_kv_size += updates.size();
    if (const auto [itr, inserted] = category_updates_.kv.try_emplace(category_id, std::move(updates.data_));
        !inserted) {
      (void)itr;  // disable unused variable
      throw std::logic_error{std::string("Only one update for category is allowed. type: Versioned, category: ") +
                             category_id};
    }
  }

  void add(const std::string& category_id, ImmutableUpdates&& updates) {
    immutable_size += updates.size();
    if (const auto [itr, inserted] = category_updates_.kv.try_emplace(category_id, std::move(updates.data_));
        !inserted) {
      (void)itr;  // disable unused variable
      throw std::logic_error{std::string("Only one update for category is allowed. type: Immutable, category: ") +
                             category_id};
    }
  }

  std::size_t size() const { return block_merkle_size + versioned_kv_size + immutable_size; }
  bool empty() const { return size() == 0; }
  std::size_t block_merkle_size{};
  std::size_t versioned_kv_size{};
  std::size_t immutable_size{};

 private:
  friend class KeyValueBlockchain;
  CategoryInput category_updates_;
};

}  // namespace concord::kvbc::categorization
