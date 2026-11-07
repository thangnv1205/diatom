/* Rewind: hold MENU+L1 and the game runs backwards.
 *
 * A ring of whole serialized states, one taken every REWIND_EVERY core frames
 * while the game runs at 1x, and stepped back through one per turn while the
 * chord is held. Whole states rather than deltas against the previous one:
 * delta coding is what RetroArch does to fit minutes of history, and it costs
 * a compare over the whole state on every capture. A handheld's 8- and 16-bit
 * cores have states of tens to a few hundred KB, so a fixed RAM budget holds
 * a useful stretch of history uncompressed, for a memcpy per capture.
 *
 * Bounded twice, and both bounds are the point:
 *
 *   REWIND_BUDGET   total bytes the ring may hold. 24 MB against 975 MB of RAM
 *                   on the Brick, beside a resident registry measured at about
 *                   15 MB (see core.c).
 *   REWIND_MAX_STATE a core whose state is bigger than this gets no rewind at
 *                   all: PlayStation's is about 4.5 MB, which the budget would
 *                   turn into five snapshots, about a second of history, for a
 *                   serialize every quarter second that costs real frame time.
 *                   A rewind that cannot go back far enough to matter is not
 *                   worth the stutter it buys.
 *
 * Allocated on the first capture of a session, sized from serialize_size()
 * then, and freed at the next session's reset: a game's state size is known
 * only once it is loaded, and some cores (mGBA) report a different size
 * before the first frame. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cheevos.h"
#include "diatom.h"

#define REWIND_EVERY      15                  /* core frames between captures  */
#define REWIND_BUDGET     (24u * 1024 * 1024) /* bytes, whole ring             */
#define REWIND_MAX_STATE  (2u * 1024 * 1024)  /* bigger and rewind stays off   */
#define REWIND_MIN_SLOTS  8                   /* fewer and it is not worth it  */

static uint8_t *g_ring;       /* g_slots * g_size bytes                    */
static size_t   g_size;       /* serialize_size() when the ring was made   */
static int      g_slots;      /* capacity                                  */
static int      g_head;       /* next slot to write                        */
static int      g_count;      /* valid snapshots, newest at g_head - 1     */
static int      g_since;      /* core frames since the last capture        */
static bool     g_disabled;   /* decided once per session                  */
static bool     g_said;       /* the "rewinding" line, once per hold       */

void diatom_rewind_reset(void)
{
	free(g_ring);
	g_ring = NULL;
	g_size = 0;
	g_slots = g_head = g_count = g_since = 0;
	g_disabled = false;
	g_said = false;
}

static bool ring_ready(diatom_core *c)
{
	size_t n;

	if (g_ring) return true;
	if (g_disabled) return false;
	if (!c || !c->serialize_size || !c->serialize || !c->unserialize) {
		g_disabled = true;
		return false;
	}
	n = c->serialize_size();
	if (n == 0 || n > REWIND_MAX_STATE) {
		printf("diatom: rewind off, state is %zu bytes\n", n);
		g_disabled = true;
		return false;
	}
	g_slots = (int)(REWIND_BUDGET / n);
	if (g_slots < REWIND_MIN_SLOTS) {
		g_disabled = true;
		return false;
	}
	g_ring = malloc((size_t)g_slots * n);
	if (!g_ring) {
		printf("diatom: rewind off, %zu bytes not available\n",
		       (size_t)g_slots * n);
		g_disabled = true;
		return false;
	}
	g_size = n;
	printf("diatom: rewind %d snapshot(s) of %zu bytes, about %.0fs\n",
	       g_slots, n, g_slots * REWIND_EVERY / 60.0);
	return true;
}

void diatom_rewind_frames(diatom_core *c, int ran)
{
	g_said = false;
	g_since += ran;
	if (g_since < REWIND_EVERY) return;
	g_since = 0;
	if (!ring_ready(c)) return;

	/* A core whose state grew (a cartridge with more save RAM mapped in after
	 * boot) would overrun its slot: stop rather than write past it. */
	if (c->serialize_size() > g_size) {
		printf("diatom: rewind off, state grew past %zu bytes\n", g_size);
		diatom_rewind_reset();
		g_disabled = true;
		return;
	}
	if (!c->serialize(g_ring + (size_t)g_head * g_size, g_size)) return;
	g_head = (g_head + 1) % g_slots;
	if (g_count < g_slots) g_count++;
}

bool diatom_rewind_step(diatom_core *c)
{
	int slot;

	if (!g_ring || g_count == 0) return false;
	if (!g_said) {
		printf("diatom: rewinding, %d snapshot(s) held\n", g_count);
		fflush(stdout);
		g_said = true;
	}
	slot = (g_head - 1 + g_slots) % g_slots;
	if (!c->unserialize(g_ring + (size_t)slot * g_size, g_size)) return false;

	/* The newest snapshot is kept while it is the only one left, so holding
	 * the chord at the bottom of history parks the game there rather than
	 * letting it run forward again under the player's thumb. */
	if (g_count > 1) {
		g_head = slot;
		g_count--;
	}
	g_since = 0;

	/* Same reason as a state load (save.c): achievement progress belongs to
	 * a timeline that no longer exists. */
	diatom_cheevos_runtime_reset();
	return true;
}

bool diatom_rewind_available(void)
{
	return !g_disabled;
}
