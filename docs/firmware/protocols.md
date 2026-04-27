# Protocol Stack

The firmware currently implements sACN (E1.31). Art-Net and ShowNet support is
planned but not yet started. Only one protocol will be active at a time, selected
in the configuration.

## Protocols

| Protocol | Module | Transport | Port | Status |
|----------|--------|-----------|------|--------|
| **sACN (E1.31)** | `src/sacn/` | UDP multicast | 5568 | Implemented |
| **Art-Net** | `src/artnet/` | UDP broadcast/unicast | 6454 | Planned |
| **ShowNet** | `src/shownet/` | UDP broadcast | 2501 | Planned |

### sACN (E1.31) -- ANSI E1.31-2018

Streaming Architecture for Control Networks. The primary protocol, used by
ETC Eos, QLC+, and most modern lighting systems.

- **Multicast range**: `239.255.{high}.{low}` where universe = `(high << 8) | low`
- **Universe range**: 1-63999
- **Sequence**: 0-255, wrapping
- [E1.31-2018 Standard](https://tsp.esta.org/tsp/documents/published_docs.php)

### Art-Net 4

Widely used DMX-over-IP protocol by Artistic Licence.

- **Universe addressing**: Net (7 bits) / SubNet (4 bits) / Universe (4 bits)
- **Max universes**: 32,768
- [Art-Net Specification](https://art-net.org.uk/resources/art-net-specification/)

### ShowNet (Strand Proprietary)

Legacy protocol used by Strand 300/500 series consoles and ShowNet nodes.
Included for backward compatibility with existing Strand installations.

- **Encoding**: Run-Length Encoding (RLE) for DMX data
- [PyShowNet](https://github.com/nickvsnetworking/PyShowNet)
- [ShowNet Reverse Engineering](https://nickvsnetworking.com/reverse-engineering-dead-protocols-strand-shownet/)
- [Open Lighting Wiki](https://wiki.openlighting.org/index.php/Shownet)

## Source Timeout

All protocols share a common source timeout of **2500 ms** (`SOURCE_TIMEOUT_MS`
in `common.h`). When no packets are received for this duration, the source is
considered lost and the firmware enters hold mode (maintaining last DMX values
for the configured hold time) or zeros the output.

## Implementation Pattern

Each protocol module follows the same interface pattern:

1. **`init()`** -- Create and bind UDP socket, join multicast group if needed
2. **Socket polling** -- The main loop includes the protocol's socket in its
   `select()` call
3. **Packet processing** -- Parse incoming packets, extract DMX data into
   `dmx_frame_t`
4. **TX support** (sACN only) -- `sacn_tx` sends DMX input data as sACN packets
