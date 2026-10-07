// ptree_fake.cpp — builds a REAL, valid empty boost::property_tree ptree
// for the Bug 27 get_child patch to return.
//
// Background: libengine.so's throwing get_child throws ptree_bad_path when a
// node (e.g. "<xmlattr>") is missing. The loader patches the not-found path to
// return an empty ptree instead. A hand-made zeroed buffer CANNOT work:
//   - basic_ptree is {data_type m_data @0; void* m_children @12} (16 bytes).
//     m_data is a 12-byte string (Android NDK libc++,
//     _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT: byte[0]=len*2 short / odd long,
//     SSO chars at [1..], long ptr at [8]).
//   - m_children is NOT an inline list — it's a heap pointer to a
//     boost::multi_index_container (sequenced + ordered_non_unique indices).
//     The default ctor does `new base_container`. There is no valid empty
//     ptree without a real container.
//   - libengine.so is stripped: no ptree ctor symbol to resolve.
//
// So we build a real one here with the SDK's Boost headers. The empty
// multi_index_container layout was verified byte-identical between Boost
// 1.55 and 1.78 (16-byte container, 48-byte header node), so the game's
// Boost (any 1.5x-1.7x) will interpret it correctly as empty.
// The game only READS this node (never deletes it — get_child returns a
// non-owning pointer), so loader-heap allocation is safe.

#include <stddef.h>
#include <stdint.h>
#include <new>
#include <utility>
#include <functional>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

// Exact replica of the game's 12-byte string.
struct game_string {
    uint32_t w0, w1, w2;
    game_string() : w0(0), w1(0), w2(0) {}
    game_string(const game_string& o) : w0(o.w0), w1(o.w1), w2(o.w2) {}
    bool operator<(const game_string& o) const {
        if (w0 != o.w0) return w0 < o.w0;
        if (w1 != o.w1) return w1 < o.w1;
        return w2 < o.w2;
    }
    bool operator==(const game_string& o) const {
        return w0 == o.w0 && w1 == o.w1 && w2 == o.w2;
    }
};
static_assert(sizeof(game_string) == 12, "game_string must be 12 bytes");

// Exact replica of the game's basic_ptree<std::string,std::string>:
// { m_data @0 (12B); m_children @12 (4B, heap container*) } = 16 bytes.
struct game_ptree {
    game_string m_data;
    void* m_children;
};
static_assert(sizeof(game_ptree) == 16, "game_ptree must be 16 bytes");
static_assert(offsetof(game_ptree, m_data) == 0, "");
static_assert(offsetof(game_ptree, m_children) == 12, "");

// The child container: same index set as boost::property_tree (1.55-1.78).
typedef std::pair<const game_string, game_ptree> value_type;
struct by_name {};
typedef boost::multi_index_container<
    value_type,
    boost::multi_index::indexed_by<
        boost::multi_index::sequenced<>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<by_name>,
            boost::multi_index::member<value_type, const game_string,
                                       &value_type::first>,
            std::less<game_string>
        >
    >
> container_type;

// Static storage for the ptree object itself (never freed).
static uint8_t ptree_storage[sizeof(game_ptree)] __attribute__((aligned(4)));
static game_ptree* fake_ptree = 0;

extern "C" void blog(const char* fmt, ...);

extern "C" void* ptree_fake_init(void) {
    if (fake_ptree)
        return fake_ptree;
    // Real container, heap-allocated exactly like the game's default ctor
    // does (basic_ptree() : m_children(new base_container)).
    container_type* c = new (std::nothrow) container_type();
    if (!c) {
        blog("ptree_fake: container alloc FAILED");
        return 0;
    }
    game_ptree* p = new (ptree_storage) game_ptree();
    p->m_data = game_string();  // all zeros = valid empty string
    p->m_children = c;
    fake_ptree = p;
    blog("ptree_fake: real empty ptree at %p (container %p, empty=%d)",
         (void*)p, (void*)c, (int)c->empty());
    return p;
}
