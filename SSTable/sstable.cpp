#include "sstable.hpp"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// Compression interface (no-op placeholder)
//
// To enable real compression swap these two functions with Snappy calls:
//   #include <snappy.h>
//   Compress:   snappy::Compress(in.data(), in.size(), &out); return out;
//   Decompress: snappy::Uncompress(in.data(), in.size(), &out); return out;
// ─────────────────────────────────────────────────────────────────────────────

static std::string Compress(const std::string& in)   { return in; }
static std::string Decompress(const std::string& in) { return in; }


// ─────────────────────────────────────────────────────────────────────────────
// Internal validation helpers
// ─────────────────────────────────────────────────────────────────────────────

// Safe narrowing: throws if value exceeds uint32_t range.
// Replaces every bare static_cast<uint32_t>(size_t) in the write path.
static uint32_t NarrowToU32(size_t v, const char* context)
{
    if (v > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error(std::string(context) +
            ": size exceeds uint32_t range (" + std::to_string(v) + ")");
    return static_cast<uint32_t>(v);
}

// Safe streamsize cast: throws if value exceeds the signed streamsize range.
// std::streamsize is signed; on 32-bit MSVC/MinGW it is 32-bit.
static std::streamsize ToStreamSize(size_t v, const char* context)
{
    if (v > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error(std::string(context) +
            ": write size exceeds std::streamsize range");
    return static_cast<std::streamsize>(v);
}

// Bounds-checked addition: throws on overflow before any comparison.
// Replaces key_size + 24 and num_restarts * 4 + 4 in the read path.
static size_t CheckedAdd(size_t a, size_t b, const char* context)
{
    if (a > std::numeric_limits<size_t>::max() - b)
        throw std::runtime_error(std::string(context) +
            ": integer overflow in size calculation");
    return a + b;
}

static size_t CheckedMul(size_t a, size_t b, const char* context)
{
    if (b != 0 && a > std::numeric_limits<size_t>::max() / b)
        throw std::runtime_error(std::string(context) +
            ": integer overflow in size calculation");
    return a * b;
}


// ─────────────────────────────────────────────────────────────────────────────
// BloomFilter
// ─────────────────────────────────────────────────────────────────────────────

BloomFilter::BloomFilter(size_t num_keys)
    : num_hashes_(num_keys == 0 ? 0 : bigtable::kBloomNumHashes)
{
    size_t bits = num_keys * bigtable::kBloomBitsPerKey;
    if (bits < 64) bits = 64;
    bits_.assign((bits + 7) / 8, 0);
}

// MurmurHash3-inspired mixing — two independent 64-bit hashes.
// Used by Kirsch-Mitzenmacher double hashing: h_i(k) = (h1 + i * h2) mod m
std::pair<uint64_t, uint64_t> BloomFilter::BaseHashes(std::string_view key)
{
    uint64_t h1 = 0x936866195423B4A5ULL;
    uint64_t h2 = 0xC4CEB9FE1A85EC53ULL;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(key.data());
    size_t len = key.size();

    while (len >= 8) {
        uint64_t chunk;
        std::memcpy(&chunk, data, 8);
        h1 ^= chunk;
        h1  = (h1 << 31) | (h1 >> 33);
        h1 *= 0xFF51AFD7ED558CCDULL;
        h2 ^= chunk;
        h2  = (h2 << 27) | (h2 >> 37);
        h2 *= 0xC4CEB9FE1A85EC53ULL;
        data += 8; len -= 8;
    }
    uint64_t tail = 0;
    for (size_t i = 0; i < len; ++i)
        tail |= static_cast<uint64_t>(data[i]) << (8 * i);
    h1 ^= tail; h2 ^= tail ^ len;
    h1 ^= h1 >> 33; h1 *= 0xFF51AFD7ED558CCDULL; h1 ^= h1 >> 33;
    h2 ^= h2 >> 33; h2 *= 0xC4CEB9FE1A85EC53ULL; h2 ^= h2 >> 33;
    return {h1, h2};
}

void BloomFilter::Add(std::string_view user_key)
{
    const size_t m = bits_.size() * 8;
    auto [h1, h2] = BaseHashes(user_key);
    for (int i = 0; i < num_hashes_; ++i) {
        uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % m;
        bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
}

std::string BloomFilter::Finish() const
{
    // Format: [bitset_size : 4B BE][bitset bytes][num_hashes : 1B]
    std::string out;
    AppendUint32BE(out, NarrowToU32(bits_.size(), "BloomFilter::Finish"));
    out.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
    out += static_cast<char>(num_hashes_);
    return out;
}

bool BloomFilter::MayContain(std::string_view serialised, std::string_view user_key)
{
    if (serialised.size() < 5) return true;
    const char* p        = serialised.data();
    uint32_t bitset_size = DecodeUint32BE(p); p += 4;
    if (serialised.size() < 4u + bitset_size + 1u) return true;
    const uint8_t* bits       = reinterpret_cast<const uint8_t*>(p);
    int            num_hashes = static_cast<uint8_t>(p[bitset_size]);
    size_t         m          = bitset_size * 8;
    if (m == 0 || num_hashes == 0) return true;
    auto [h1, h2] = BaseHashes(user_key);
    for (int i = 0; i < num_hashes; ++i) {
        uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % m;
        if (!(bits[bit / 8] & (1u << (bit % 8)))) return false;
    }
    return true;
}


// ─────────────────────────────────────────────────────────────────────────────
// SSTableWriter
// ─────────────────────────────────────────────────────────────────────────────

SSTableWriter::SSTableWriter(const std::string& path)
    : impl_(std::make_unique<Impl>())
{
    impl_->file_.open(path, std::ios::binary | std::ios::trunc);
    if (!impl_->file_.is_open())
        throw std::runtime_error("SSTableWriter: cannot open file: " + path);
}

SSTableWriter::~SSTableWriter() = default;

// WriteRaw: writes exactly data.size() bytes or throws.
// Checks both the streamsize cast and the post-write stream state.
void SSTableWriter::WriteRaw(const std::string& data)
{
    if (data.empty()) return;
    std::streamsize n = ToStreamSize(data.size(), "SSTableWriter::WriteRaw");
    impl_->file_.write(data.data(), n);
    if (!impl_->file_)
        throw std::runtime_error("SSTableWriter: write failed (disk full or I/O error)");
    impl_->file_offset_ += data.size();
}

// ── FlushBlock ────────────────────────────────────────────────────────────────
void SSTableWriter::FlushBlock()
{
    if (impl_->entry_count_ == 0) return;

    // Append restart trailer: [restart_0:4B]...[restart_k:4B][count:4B]
    std::string& buf = impl_->block_buf_;
    for (uint32_t offset : impl_->restarts_) AppendUint32BE(buf, offset);
    AppendUint32BE(buf, NarrowToU32(impl_->restarts_.size(),
                                    "SSTableWriter::FlushBlock restart count"));

    // Compress and write data block.
    std::string compressed = Compress(buf);
    BlockHandle handle;
    handle.offset = impl_->file_offset_;
    handle.size   = NarrowToU32(compressed.size(),
                                 "SSTableWriter::FlushBlock compressed block size");
    WriteRaw(compressed);

    // Write paired Bloom filter block.
    std::string filter_bytes = impl_->bloom_->Finish();
    handle.filter_offset = impl_->file_offset_;
    handle.filter_size   = NarrowToU32(filter_bytes.size(),
                                        "SSTableWriter::FlushBlock filter size");
    WriteRaw(filter_bytes);

    // Decode last key for index binary search.
    InternalKey last_decoded;
    if (!InternalKey::Decode(impl_->last_key_, last_decoded))
        throw std::runtime_error("SSTableWriter: failed to decode last key during flush");
    handle.last_key = std::move(last_decoded);
    impl_->index_.push_back(std::move(handle));

    // Reset block state.
    impl_->block_buf_.clear();
    impl_->last_key_.clear();
    impl_->entry_count_ = 0;
    impl_->restarts_.clear();
    impl_->bloom_.reset();
}

// ── Add ───────────────────────────────────────────────────────────────────────
void SSTableWriter::Add(const InternalKey& key, const std::string& value)
{
    assert(!impl_->finished_);

    std::string encoded_key = key.Encode();

    if (!impl_->bloom_)
        impl_->bloom_ = std::make_unique<BloomFilter>(bigtable::kSSTableBlockSize / 32 + 1);

    bool   is_restart = (impl_->entry_count_ % bigtable::kRestartInterval == 0);
    size_t shared     = 0;
    if (!is_restart && !impl_->last_key_.empty())
        shared = SharedPrefixLen(impl_->last_key_, encoded_key);

    if (is_restart)
        impl_->restarts_.push_back(NarrowToU32(impl_->block_buf_.size(),
                                                "SSTableWriter::Add restart offset"));

    // Entry format: [shared:4B BE][unshared:4B BE][val_len:4B BE][key_delta][value]
    uint32_t unshared = NarrowToU32(encoded_key.size() - shared, "SSTableWriter::Add unshared");
    uint32_t val_len  = NarrowToU32(value.size(),                "SSTableWriter::Add val_len");
    AppendUint32BE(impl_->block_buf_, NarrowToU32(shared, "SSTableWriter::Add shared"));
    AppendUint32BE(impl_->block_buf_, unshared);
    AppendUint32BE(impl_->block_buf_, val_len);
    impl_->block_buf_.append(encoded_key.data() + shared, unshared);
    impl_->block_buf_.append(value);

    impl_->bloom_->Add(key.UserKey());
    impl_->last_key_    = std::move(encoded_key);
    impl_->entry_count_++;
    impl_->num_entries_++;

    if (impl_->block_buf_.size() >= bigtable::kSSTableBlockSize)
        FlushBlock();
}

// ── Finish ────────────────────────────────────────────────────────────────────
void SSTableWriter::Finish()
{
    assert(!impl_->finished_);
    impl_->finished_ = true;

    FlushBlock();

    if (impl_->index_.empty())
        throw std::runtime_error("SSTableWriter::Finish called on empty table");

    // Write index block.
    uint64_t index_offset = impl_->file_offset_;
    std::string index_buf;
    AppendUint32BE(index_buf, NarrowToU32(impl_->index_.size(),
                                           "SSTableWriter::Finish num_blocks"));
    for (const BlockHandle& h : impl_->index_) {
        std::string encoded = h.last_key.Encode();
        AppendUint32BE(index_buf, NarrowToU32(encoded.size(),
                                               "SSTableWriter::Finish index key size"));
        index_buf.append(encoded);
        AppendUint64BE(index_buf, h.offset);
        AppendUint32BE(index_buf, h.size);
        AppendUint64BE(index_buf, h.filter_offset);
        AppendUint32BE(index_buf, h.filter_size);
    }
    WriteRaw(index_buf);

    // Write footer.
    Footer footer;
    footer.index_offset = index_offset;
    footer.index_size   = NarrowToU32(index_buf.size(),
                                       "SSTableWriter::Finish index_size");
    footer.num_blocks   = NarrowToU32(impl_->index_.size(),
                                       "SSTableWriter::Finish footer num_blocks");
    WriteRaw(footer.Encode());

    // Flush and verify the stream is healthy.
    impl_->file_.flush();
    if (!impl_->file_)
        throw std::runtime_error("SSTableWriter::Finish: flush failed");
}

uint64_t SSTableWriter::NumEntries() const { return impl_->num_entries_; }
uint64_t SSTableWriter::FileSize()   const { return impl_->file_offset_; }
size_t   SSTableWriter::NumBlocks()  const { return impl_->index_.size(); }


// ─────────────────────────────────────────────────────────────────────────────
// SSTableReader
// ─────────────────────────────────────────────────────────────────────────────

SSTableReader::~SSTableReader() = default;

SSTableReader::SSTableReader(const std::string& path)
    : impl_(std::make_unique<Impl>())
{
    impl_->file_.open(path, std::ios::binary);
    if (!impl_->file_.is_open())
        throw std::runtime_error("SSTableReader: cannot open file: " + path);

    // Determine file size — check seekg and tellg independently.
    impl_->file_.seekg(0, std::ios::end);
    if (!impl_->file_)
        throw std::runtime_error("SSTableReader: seekg(end) failed");

    std::streampos pos = impl_->file_.tellg();
    if (pos == std::streampos(-1))
        throw std::runtime_error("SSTableReader: tellg() failed");
    impl_->file_size_ = static_cast<uint64_t>(pos);

    if (impl_->file_size_ < bigtable::kFooterSize)
        throw std::runtime_error("SSTableReader: file too small to contain footer ("
            + std::to_string(impl_->file_size_) + " bytes)");

    // Read and validate footer.
    std::string footer_buf = ReadAt(
        impl_->file_size_ - bigtable::kFooterSize,
        static_cast<uint32_t>(bigtable::kFooterSize));  // 28 — always fits uint32_t
    Footer footer;
    if (!Footer::Decode(footer_buf, footer))
        throw std::runtime_error("SSTableReader: corrupt footer (magic mismatch)");
    if (footer.version != bigtable::kSSTableVersion)
        throw std::runtime_error("SSTableReader: unsupported SSTable version "
            + std::to_string(footer.version)
            + " (expected " + std::to_string(bigtable::kSSTableVersion) + ")");

    // Validate index block bounds before reading.
    // index_offset + index_size must not exceed file_size - kFooterSize.
    uint64_t footer_start = impl_->file_size_ - bigtable::kFooterSize;
    if (footer.index_offset > footer_start)
        throw std::runtime_error("SSTableReader: index_offset past footer start");
    if (footer.index_size > footer_start - footer.index_offset)
        throw std::runtime_error("SSTableReader: index block extends into footer");

    // Read and parse index block.
    std::string index_buf = ReadAt(footer.index_offset, footer.index_size);
    const char* p   = index_buf.data();
    const char* end = p + index_buf.size();

    if (end - p < 4)
        throw std::runtime_error("SSTableReader: index block too short");

    uint32_t num_blocks = DecodeUint32BE(p); p += 4;

    // Sanity cap: a 64MB SSTable with 4KB blocks has at most 16384 blocks.
    // A larger claim almost certainly means a corrupt index.
    constexpr uint32_t kMaxSaneBlocks =
        static_cast<uint32_t>(bigtable::kSSTableTargetFileSize / bigtable::kSSTableBlockSize);
    if (num_blocks > kMaxSaneBlocks)
        throw std::runtime_error("SSTableReader: implausible block count "
            + std::to_string(num_blocks)
            + " (max sane: " + std::to_string(kMaxSaneBlocks) + ")");

    impl_->index_.reserve(num_blocks);

    for (uint32_t i = 0; i < num_blocks; ++i) {
        if (end - p < 4)
            throw std::runtime_error("SSTableReader: truncated index block at entry "
                + std::to_string(i));

        uint32_t key_size = DecodeUint32BE(p); p += 4;

        // Overflow-safe bounds check: key_size + 24 must not wrap.
        // (24 = 8+4+8+4 bytes for offset/size/filter_offset/filter_size)
        size_t entry_tail = CheckedAdd(static_cast<size_t>(key_size), 24,
                                        "SSTableReader: index entry size");
        if (static_cast<size_t>(end - p) < entry_tail)
            throw std::runtime_error("SSTableReader: truncated index entry "
                + std::to_string(i));

        BlockHandle h;
        if (!InternalKey::Decode(std::string_view(p, key_size), h.last_key))
            throw std::runtime_error("SSTableReader: corrupt index key at entry "
                + std::to_string(i));
        p += key_size;

        h.offset        = DecodeUint64BE(p); p += 8;
        h.size          = DecodeUint32BE(p); p += 4;
        h.filter_offset = DecodeUint64BE(p); p += 8;
        h.filter_size   = DecodeUint32BE(p); p += 4;

        // Validate block extents lie within the file.
        if (h.offset > impl_->file_size_ ||
            h.size   > impl_->file_size_ - h.offset)
            throw std::runtime_error("SSTableReader: data block " + std::to_string(i)
                + " extends outside file");
        if (h.filter_offset > impl_->file_size_ ||
            h.filter_size   > impl_->file_size_ - h.filter_offset)
            throw std::runtime_error("SSTableReader: filter block " + std::to_string(i)
                + " extends outside file");

        impl_->index_.push_back(std::move(h));
    }

    // Smallest key = first entry of first data block.
    if (!impl_->index_.empty()) {
        auto first_block = ReadDataBlock(0);
        if (!first_block.empty())
            impl_->smallest_key_ = first_block.front().first;
        impl_->largest_key_ = impl_->index_.back().last_key;
    }
}

// ── ReadAt ────────────────────────────────────────────────────────────────────
// Reads exactly `size` bytes from `offset`. Throws on any I/O failure
// including partial reads (gcount() < size).
std::string SSTableReader::ReadAt(uint64_t offset, uint32_t size)
{
    if (size == 0) return {};

    // std::streamoff is signed. Guard against overflow on platforms where it
    // is 32-bit (e.g. 32-bit MSVC) or when given a corrupt offset value.
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
        throw std::runtime_error("SSTableReader::ReadAt: offset " +
            std::to_string(offset) + " exceeds std::streamoff range");

    impl_->file_.seekg(static_cast<std::streamoff>(offset));
    if (!impl_->file_)
        throw std::runtime_error("SSTableReader: seekg failed at offset "
            + std::to_string(offset));

    std::string buf(size, '\0');
    std::streamsize n = ToStreamSize(size, "SSTableReader::ReadAt");
    impl_->file_.read(buf.data(), n);

    // Check both stream state and byte count — read() can succeed partially.
    if (!impl_->file_ ||
        impl_->file_.gcount() != static_cast<std::streamsize>(size))
        throw std::runtime_error("SSTableReader: partial or failed read at offset "
            + std::to_string(offset) + " (requested " + std::to_string(size)
            + ", got " + std::to_string(impl_->file_.gcount()) + ")");

    return buf;
}

// ── ReadDataBlock ─────────────────────────────────────────────────────────────
std::vector<std::pair<InternalKey, std::string>>
SSTableReader::ReadDataBlock(size_t block_idx)
{
    const BlockHandle& h   = impl_->index_[block_idx];
    std::string compressed = ReadAt(h.offset, h.size);
    std::string raw        = Decompress(compressed);

    if (raw.size() < 4)
        throw std::runtime_error("SSTableReader: data block too small");

    // Overflow-safe trailer size: num_restarts * 4 + 4
    uint32_t num_restarts = DecodeUint32BE(raw.data() + raw.size() - 4);
    size_t   trailer_size = CheckedAdd(
        CheckedMul(static_cast<size_t>(num_restarts), 4,
                    "SSTableReader: restart trailer mul"),
        4, "SSTableReader: restart trailer add");

    if (raw.size() < trailer_size)
        throw std::runtime_error("SSTableReader: data block smaller than restart trailer");

    const char* data     = raw.data();
    size_t      data_end = raw.size() - trailer_size;
    size_t      pos      = 0;

    std::vector<std::pair<InternalKey, std::string>> entries;
    std::string current_key;

    while (pos < data_end) {
        if (data_end - pos < 12)
            throw std::runtime_error("SSTableReader: truncated entry header at pos "
                + std::to_string(pos));

        uint32_t shared   = DecodeUint32BE(data + pos); pos += 4;
        uint32_t unshared = DecodeUint32BE(data + pos); pos += 4;
        uint32_t val_len  = DecodeUint32BE(data + pos); pos += 4;

        // Overflow-safe: unshared + val_len must not wrap.
        size_t need = CheckedAdd(static_cast<size_t>(unshared),
                                  static_cast<size_t>(val_len),
                                  "SSTableReader: entry data size");
        if (data_end - pos < need)
            throw std::runtime_error("SSTableReader: truncated entry data at pos "
                + std::to_string(pos));

        // shared must not exceed the current reconstructed key length.
        if (static_cast<size_t>(shared) > current_key.size())
            throw std::runtime_error("SSTableReader: shared prefix exceeds current key "
                "size (corrupt block)");

        current_key.resize(shared);
        current_key.append(data + pos, unshared);
        pos += unshared;

        std::string value(data + pos, val_len);
        pos += val_len;

        InternalKey key;
        if (!InternalKey::Decode(current_key, key))
            throw std::runtime_error("SSTableReader: corrupt entry key at pos "
                + std::to_string(pos));

        entries.emplace_back(std::move(key), std::move(value));
    }

    return entries;
}

// ── FindBlock ─────────────────────────────────────────────────────────────────
size_t SSTableReader::FindBlock(const InternalKey& seek_key) const
{
    InternalKeyComparator cmp;
    size_t lo = 0, hi = impl_->index_.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cmp(impl_->index_[mid].last_key, seek_key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// ── Get ───────────────────────────────────────────────────────────────────────
GetResult SSTableReader::Get(const std::string& row,
                             const std::string& col,
                             std::string&       value_out)
{
    if (impl_->index_.empty()) return GetResult::kNotFound;

    if (row < impl_->smallest_key_.row) return GetResult::kNotFound;
    if (row > impl_->largest_key_.row)  return GetResult::kNotFound;

    InternalKey seek_key(row, col, bigtable::kMaxTimestamp, kTypeValue);

    size_t block_idx = FindBlock(seek_key);
    if (block_idx >= impl_->index_.size()) return GetResult::kNotFound;

    std::string user_key   = seek_key.UserKey();
    const BlockHandle& h   = impl_->index_[block_idx];
    std::string filter_buf = ReadAt(h.filter_offset, h.filter_size);
    if (!BloomFilter::MayContain(filter_buf, user_key)) return GetResult::kNotFound;

    auto entries = ReadDataBlock(block_idx);
    for (const auto& [key, value] : entries) {
        if (key.row != row || key.col != col) continue;
        if (key.type == kTypeDeletion) return GetResult::kDeleted;
        value_out = value;
        return GetResult::kFound;
    }
    return GetResult::kNotFound;
}

const InternalKey& SSTableReader::SmallestKey() const { return impl_->smallest_key_; }
const InternalKey& SSTableReader::LargestKey()  const { return impl_->largest_key_; }
uint64_t           SSTableReader::FileSize()    const { return impl_->file_size_; }
size_t             SSTableReader::NumBlocks()   const { return impl_->index_.size(); }

bool SSTableReader::MayContain(const std::string& row, const std::string& /*col*/) const
{
    if (impl_->index_.empty()) return false;
    if (row < impl_->smallest_key_.row) return false;
    if (row > impl_->largest_key_.row)  return false;
    return true;
}


// ─────────────────────────────────────────────────────────────────────────────
// SSTableReader::Iterator
// ─────────────────────────────────────────────────────────────────────────────

SSTableReader::Iterator::Iterator(SSTableReader* reader)
    : reader_(reader), block_idx_(0), entry_idx_(0)
{}

void SSTableReader::Iterator::LoadBlock(size_t block_idx)
{
    entries_   = reader_->ReadDataBlock(block_idx);
    block_idx_ = block_idx;
    entry_idx_ = 0;
}

bool SSTableReader::Iterator::Valid() const
{
    return block_idx_ < reader_->impl_->index_.size()
        && entry_idx_ < entries_.size();
}

const InternalKey& SSTableReader::Iterator::key()   const { assert(Valid()); return entries_[entry_idx_].first; }
const std::string& SSTableReader::Iterator::value() const { assert(Valid()); return entries_[entry_idx_].second; }

void SSTableReader::Iterator::Next()
{
    assert(Valid());
    ++entry_idx_;
    if (entry_idx_ >= entries_.size()) {
        ++block_idx_;
        if (block_idx_ < reader_->impl_->index_.size())
            LoadBlock(block_idx_);
        else {
            entries_.clear();
            entry_idx_ = 0;
        }
    }
}

void SSTableReader::Iterator::SeekToFirst()
{
    if (reader_->impl_->index_.empty()) {
        entries_.clear(); block_idx_ = 0; entry_idx_ = 0;
        return;
    }
    LoadBlock(0);
}

void SSTableReader::Iterator::Seek(const InternalKey& target)
{
    size_t idx = reader_->FindBlock(target);
    if (idx >= reader_->impl_->index_.size()) {
        entries_.clear();
        block_idx_ = reader_->impl_->index_.size();
        entry_idx_ = 0;
        return;
    }
    LoadBlock(idx);

    InternalKeyComparator cmp;
    while (entry_idx_ < entries_.size() &&
           cmp(entries_[entry_idx_].first, target) < 0)
        ++entry_idx_;

    if (entry_idx_ >= entries_.size()) {
        ++block_idx_;
        if (block_idx_ < reader_->impl_->index_.size())
            LoadBlock(block_idx_);
        else
            entries_.clear();
    }
}

SSTableReader::Iterator SSTableReader::NewIterator()
{
    return Iterator(this);
}