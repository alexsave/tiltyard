#ifndef TEST_COPY_RANGE_H
#define TEST_COPY_RANGE_H

#include <assert.h>
#include <stdlib.h>
#include "ob.h"
#include "order.h"
#include "constants.h"
#include "fl.h"
#include "cb.h"
#include "fill.h"
#include "test_book_invariants.h" // book_ok

// ob_copy_range used to walk one level at a time. it now finds the levels a watched cancel lands
// on, bulk-copies the untouched runs between them in single memcpys, and rebases each run's
// byte_offsets by one shared delta. that delta is the whole risk: it changes every time a level
// ahead of it shrinks (a splice) or disappears (a sole cancel), so the cases worth pinning are the
// ones where a cut sits at the start, the middle and the end of a range, and where a range has two.
//
// prices and quantities live in the index array and would survive a wrong delta untouched - it is
// only the ORDER IDS, read out of the payload block, that go wrong. so every assert here reads
// through to the payload rather than trusting the index.

// every level's payload must start exactly where the previous one ended. nothing else checks this,
// and it is the single thing a bad rebase breaks
static void cr_offsets_ok(void* mbo_raw, const char* tag) {
    MBO* mbo = (MBO*)mbo_raw;
    void* data = mbo_data_start(mbo_raw);
    u32 expect = 0;

    for (u16 i = 0; i < mbo->level_count; i++) {
        if (mbo->levels[i].byte_offset != expect) {
            printf("OFFSET-BAD [%s] level %u price %u offset %u expected %u\n",
                   tag, i, mbo->levels[i].price, mbo->levels[i].byte_offset, expect);
            assert(0);
        }
        MBOLevel* lv = (MBOLevel*)(data + mbo->levels[i].byte_offset);
        expect += sizeof(MBOLevel) + lv->order_count * sizeof(MBOEntry);
    }
}

// find a level by price and read its payload back. returns order_count, fills ids_out
static u16 cr_level(void* mbo_raw, u16 price, u32* ids_out) {
    MBO* mbo = (MBO*)mbo_raw;
    void* data = mbo_data_start(mbo_raw);

    for (u16 i = 0; i < mbo->level_count; i++) {
        if (mbo->levels[i].price != price)
            continue;
        MBOLevel* lv = (MBOLevel*)(data + mbo->levels[i].byte_offset);
        for (u16 o = 0; o < lv->order_count; o++)
            ids_out[o] = lv->entries[o].order_id;
        return lv->order_count;
    }
    return 0;
}

// like build_book, but per_level orders on each level instead of one, so a cancel can be spliced
// out of a level without wiping it. ids_out is row-major: level i's orders at [i * per_level]
static void cr_build(FL* orders, void* mbo_raw, const u16* prices, u16 n, u16 hbi,
                     u16 per_level, u32* ids_out) {
    MBO* mbo = (MBO*)mbo_raw;
    mbo->level_count = n;
    mbo->hi_bid_index = hbi;

    void* data = mbo_data_start(mbo_raw);
    u32 offset = 0;

    for (u16 i = 0; i < n; i++) {
        u8 is_buy = hbi != MAX_U16 && i <= hbi;
        MBOLevel* lv = (MBOLevel*)(data + offset);
        lv->order_count = per_level;

        u32 level_qty = 0;
        for (u16 o = 0; o < per_level; o++) {
            Order ord = {0};
            ord.price = prices[i];
            ord.quantity = 100;
            ord.status = is_buy ? (1 << BUY_DIRECTION_BIT) : 0;
            u32 id = fl_insert(orders, &ord);
            ids_out[i * per_level + o] = id;
            lv->entries[o].order_id = id;
            lv->entries[o].quantity = 100;
            level_qty += 100;
        }

        mbo->levels[i].price = prices[i];
        mbo->levels[i].quantity = level_qty;
        mbo->levels[i].byte_offset = offset;
        offset += sizeof(MBOLevel) + per_level * sizeof(MBOEntry);
    }
}

// a long untouched range with no cancel at all - the pure bulk path, no cuts. every level in the
// book except the one the op joins is carried by one memcpy, so if the span length or the rebase
// is wrong this fails immediately
void test_copy_range_no_cut() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u16 prices[10] = { 80, 82, 84, 86, 88, 100, 102, 104, 106, 108 };
    u32 ids[20];
    cr_build(orders, old, prices, 10, 4, 2, ids);

    // join the top ask, cancelling nothing - both copy ranges are pure spans
    Order rep = {0};
    rep.price = 108; rep.quantity = 100;
    u32 rep_id = fl_insert(orders, &rep);
    ob_canrep(orders, rep_id, old, new, fills);

    MBO* nm = (MBO*)new;
    assert(nm->level_count == 10);
    assert(book_ok(orders, new, "no-cut"));
    cr_offsets_ok(new, "no-cut");

    // every untouched level still carries its own two orders, in order
    for (u16 i = 0; i < 9; i++) {
        u32 got[4];
        assert(cr_level(new, prices[i], got) == 2);
        assert(got[0] == ids[i * 2]);
        assert(got[1] == ids[i * 2 + 1]);
    }

    free(old); free(new); fl_free(orders); cb_free(fills);
    printf("test_copy_range_no_cut passed\n");
}

// a splice in the MIDDLE of a range. the level shrinks by one entry, so every level above it
// rebases by a different delta than the levels below - two spans, two deltas, one book
void test_copy_range_cut_middle() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u16 prices[10] = { 80, 82, 84, 86, 88, 100, 102, 104, 106, 108 };
    u32 ids[20];
    cr_build(orders, old, prices, 10, 4, 2, ids);

    // join the top ask while pulling one of the two orders resting at 84 - mid-range, not sole
    u32 victim = ids[2 * 2 + 1];
    Order rep = {0};
    rep.price = 108; rep.quantity = 100;
    rep.status = (1 << CAN_REP_BIT);
    rep.other_id = victim;
    u32 rep_id = fl_insert(orders, &rep);
    ob_canrep(orders, rep_id, old, new, fills);

    MBO* nm = (MBO*)new;
    assert(nm->level_count == 10);   // 84 keeps its other order, so no level is lost
    assert(book_ok(orders, new, "cut-middle"));
    cr_offsets_ok(new, "cut-middle");

    // the spliced level lost exactly the cancelled order
    u32 got[4];
    assert(cr_level(new, 84, got) == 1);
    assert(got[0] == ids[2 * 2]);

    // levels below the cut are untouched, levels above it rebased and still correct
    assert(cr_level(new, 82, got) == 2 && got[0] == ids[1 * 2] && got[1] == ids[1 * 2 + 1]);
    assert(cr_level(new, 86, got) == 2 && got[0] == ids[3 * 2] && got[1] == ids[3 * 2 + 1]);
    assert(cr_level(new, 106, got) == 2 && got[0] == ids[8 * 2] && got[1] == ids[8 * 2 + 1]);

    free(old); free(new); fl_free(orders); cb_free(fills);
    printf("test_copy_range_cut_middle passed\n");
}

// the cut sits on the FIRST level of the range - the leading span is empty. an off-by-one in the
// span length would read a level that is not there
void test_copy_range_cut_first() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u16 prices[8] = { 80, 82, 84, 86, 100, 102, 104, 106 };
    u32 ids[16];
    cr_build(orders, old, prices, 8, 3, 2, ids);

    u32 victim = ids[0 * 2 + 1];   // level 0, the very first thing the range copies
    Order rep = {0};
    rep.price = 106; rep.quantity = 100;
    rep.status = (1 << CAN_REP_BIT);
    rep.other_id = victim;
    u32 rep_id = fl_insert(orders, &rep);
    ob_canrep(orders, rep_id, old, new, fills);

    assert(((MBO*)new)->level_count == 8);
    assert(book_ok(orders, new, "cut-first"));
    cr_offsets_ok(new, "cut-first");

    u32 got[4];
    assert(cr_level(new, 80, got) == 1 && got[0] == ids[0]);
    assert(cr_level(new, 82, got) == 2 && got[0] == ids[1 * 2] && got[1] == ids[1 * 2 + 1]);

    free(old); free(new); fl_free(orders); cb_free(fills);
    printf("test_copy_range_cut_first passed\n");
}

// the cut sits on the LAST level of the range, so the trailing span is empty
void test_copy_range_cut_last() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u16 prices[8] = { 80, 82, 84, 86, 100, 102, 104, 106 };
    u32 ids[16];
    cr_build(orders, old, prices, 8, 3, 2, ids);

    // the op joins 106 (level 7), so the lower range is [0,7) and level 6 is its last entry
    u32 victim = ids[6 * 2 + 1];
    Order rep = {0};
    rep.price = 106; rep.quantity = 100;
    rep.status = (1 << CAN_REP_BIT);
    rep.other_id = victim;
    u32 rep_id = fl_insert(orders, &rep);
    ob_canrep(orders, rep_id, old, new, fills);

    assert(((MBO*)new)->level_count == 8);
    assert(book_ok(orders, new, "cut-last"));
    cr_offsets_ok(new, "cut-last");

    u32 got[4];
    assert(cr_level(new, 104, got) == 1 && got[0] == ids[6 * 2]);
    assert(cr_level(new, 102, got) == 2 && got[0] == ids[5 * 2] && got[1] == ids[5 * 2 + 1]);

    free(old); free(new); fl_free(orders); cb_free(fills);
    printf("test_copy_range_cut_last passed\n");
}

// a SOLE cancel mid-range: the level vanishes entirely rather than shrinking. the span after it
// has to rebase across a level that no longer exists, and the level count drops by one
void test_copy_range_sole_cut_drops_level() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    u16 prices[8] = { 80, 82, 84, 86, 100, 102, 104, 106 };
    u32 ids[8];
    cr_build(orders, old, prices, 8, 3, 1, ids);   // one order per level, so any cancel is sole

    u32 victim = ids[2];   // the 84 bid, alone on its level
    Order rep = {0};
    rep.price = 106; rep.quantity = 100;
    rep.status = (1 << CAN_REP_BIT);
    rep.other_id = victim;
    u32 rep_id = fl_insert(orders, &rep);
    ob_canrep(orders, rep_id, old, new, fills);

    MBO* nm = (MBO*)new;
    assert(nm->level_count == 7);          // 84 is gone
    assert(nm->hi_bid_index == 2);         // one fewer bid below the boundary
    assert(book_ok(orders, new, "sole-cut"));
    cr_offsets_ok(new, "sole-cut");

    u32 got[4];
    assert(cr_level(new, 84, got) == 0);   // really gone, not merely emptied
    assert(cr_level(new, 82, got) == 1 && got[0] == ids[1]);
    assert(cr_level(new, 86, got) == 1 && got[0] == ids[3]);
    assert(cr_level(new, 104, got) == 1 && got[0] == ids[6]);

    free(old); free(new); fl_free(orders); cb_free(fills);
    printf("test_copy_range_sole_cut_drops_level passed\n");
}

// TWO cuts inside one range - a pair whose legs both rest at the top of the book while cancelling
// two separate levels down at the bottom. that is three spans and two cuts in a single call, and
// it is the only case that exercises the cut ordering
void test_copy_range_two_cuts_one_range() {
    void* old = calloc(1, 1 << 20);
    void* new = calloc(1, 1 << 20);
    FL* orders = fl_init(sizeof(Order), MAX_U32);
    CB* fills = cb_init(sizeof(Fill));

    // bids 80..88, asks 100..108, two orders on every level
    u16 prices[10] = { 80, 82, 84, 86, 88, 100, 102, 104, 106, 108 };
    u32 ids[20];
    cr_build(orders, old, prices, 10, 4, 2, ids);

    // the ask leg pulls an order at 82, the bid leg pulls one at 86 - deliberately out of order,
    // so the higher-indexed cut is discovered first and has to be sorted behind the lower one
    u32 ask_victim = ids[1 * 2 + 1];   // level 1, price 82
    u32 bid_victim = ids[3 * 2 + 1];   // level 3, price 86

    Order bid = {0};
    bid.price = 88; bid.quantity = 100;
    bid.status = (1 << BUY_DIRECTION_BIT) | (1 << CAN_REP_BIT);
    bid.other_id = bid_victim;
    u32 bid_id = fl_insert(orders, &bid);

    Order ask = {0};
    ask.price = 100; ask.quantity = 100;
    ask.status = (1 << CAN_REP_BIT);
    ask.other_id = ask_victim;
    u32 ask_id = fl_insert(orders, &ask);

    ob_pair(orders, bid_id, ask_id, old, new, fills);

    MBO* nm = (MBO*)new;
    assert(nm->level_count == 10);   // both spliced levels keep their other order
    assert(book_ok(orders, new, "two-cuts"));
    cr_offsets_ok(new, "two-cuts");

    u32 got[4];
    assert(cr_level(new, 82, got) == 1 && got[0] == ids[1 * 2]);   // first cut spliced
    assert(cr_level(new, 86, got) == 1 && got[0] == ids[3 * 2]);   // second cut spliced
    assert(cr_level(new, 80, got) == 2);                            // span before both
    assert(cr_level(new, 84, got) == 2 && got[0] == ids[2 * 2]);    // span between them
    assert(cr_level(new, 104, got) == 2 && got[0] == ids[7 * 2]);   // span after both

    free(old); free(new); fl_free(orders); cb_free(fills);
    printf("test_copy_range_two_cuts_one_range passed\n");
}

#endif
