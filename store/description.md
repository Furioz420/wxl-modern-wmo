# Modern WMO

> The client's world-object loader walks each chunk positionally: it assumes the file's chunks arrive in
> exactly the order and shape it shipped with. An unfamiliar chunk left in place doesn't get skipped, it
> desynchronizes the entire parse. A world map object (WMO) authored for a later version of the game adds
> chunks the client has no slot for.

This module reads that newer file **directly**: a tag-driven walker replaces the positional one, matching
each chunk by its own marker instead of assuming a fixed order, so a chunk the client doesn't recognize is
simply skipped rather than throwing the rest of the parse off. The root file and every group file end up
filled in exactly the shape the client's own rendering, collision and portal code already expects.

There is no conversion step and no intermediate file. The bytes stay what they are; only how they're read
changes.

---

## Loading is instant

A WMO's root and group reads run in the background instead of blocking the frame that requests them. The
walk (and everything the client's own load path does with its result) is deferred to a worker thread, with
the main thread only ever touching a state flag; the client's own draw and streaming code never sees a
half-loaded object because nothing reads it until the flag says it's ready.

## Materials

FileDataID texture references are resolved to a client path (a name table needs no resolver at all). A
modern-only shader family collapses onto the closest of the client's own six, the same way a texture-less
second slot already does natively.

### Four diffuse layers instead of one

A modern material can blend four diffuse layers, each with its own UV set and a per-vertex weight
modulated by a height map. The client's shader only ever samples one. Rather than flattening the material
down to its base layer, the stock single-layer shader pair is patched in memory (disassembled, given
pass-through outputs for the extra data, reassembled) and swapped in for the batch; the extra layers ride
a second vertex stream attached for the draw and detached right after. Every prerequisite has to hold --
if a single layer texture doesn't resolve, the batch quietly draws with the stock single-layer shader
instead.

### The composite overlay wasn't blending by its own alpha

A modern material's second texture is usually a mostly-transparent detail overlay meant to composite over
the base layer by its own alpha. The client's composite shader blends by vertex-colour alpha instead,
which mixes in the overlay's dark body and turns highlight lines into dark stripes. Patched the same way:
the one blend instruction in the stock shader is rewritten in memory, scoped to modern materials only, so
a stock two-layer WMO keeps the blend it was authored for.

## Outdoor visibility

The client reopens the exterior view in exactly one way: crossing a portal into an outdoor group. Modern
content adds other ways an indoor space can still show the outside, most commonly an open-sided space like
the underside of a bridge or an archway. Left alone, walking into one of those makes half the world vanish
that was never meant to. The added rule recognizes the same case and reopens the exterior the client's own
way, with no file rewritten and nothing about the portal system reimplemented -- it just raises the same
value stock portal traversal would have.

A related fix gives the client's occlusion test a floor: a modern antiportal is often a floating slab (a
bridge deck), and the stock test treats any occluder as reaching the ground, culling everything visible
underneath it too. The same occluder edges are projected at the group's actual floor, and anything proven
to sit below that floor is restored. It can only ever un-cull, so a miss costs overdraw, never missing
geometry.

## Safety

Every fix falls back to stock behavior rather than failing loudly: an unresolvable texture, a shader that
doesn't disassemble to the expected shape, or a material outside every rule above just draws the way the
client would have drawn it natively. Session counters for each of these paths are readable from Lua.

## Interfaces

- Reads `wxl.fdid` to resolve texture FileDataIDs. With nothing supplying it, name-table materials still
  work and FileDataID-only ones simply don't resolve.
- Reads `wxl.m2draw` (published by wxl-modern-m2) to attach the four-layer material's extra vertex stream
  to a batch's native draw call.
- Reads `wxl.loadpool` to run root/group walks off the main thread. Without it, loading falls back to
  running the same walk inline, synchronously.

## Requirements

WarcraftXL on a 3.3.5a client, build **12340**. The module refuses to load against anything else rather
than guessing, and says so in the log.
