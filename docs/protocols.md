# Protocol References

## sACN (E1.31) — ANSI E1.31-2018

Streaming Architecture for Control Networks. The primary target protocol.

### Key Details
- **Transport**: UDP multicast
- **Port**: 5568
- **Multicast range**: 239.255.{high}.{low} where universe = (high << 8) | low
- **Universe range**: 1-63999
- **Max DMX slots per packet**: 512

### Packet Structure (simplified)
```
Root Layer (38 bytes):
  Preamble Size (2): 0x0010
  Post-amble Size (2): 0x0000
  ACN Packet Identifier (12): "ASC-E1.17\0\0\0"
  Flags and Length (2)
  Vector (4): VECTOR_ROOT_E131_DATA = 0x00000004
  CID (16): Component Identifier (UUID)

Framing Layer (77 bytes):
  Flags and Length (2)
  Vector (4): VECTOR_E131_DATA_PACKET = 0x00000002
  Source Name (64): Human-readable source name
  Priority (1): 0-200 (default 100)
  Sync Address (2)
  Sequence Number (1): 0-255, wrapping
  Options (1): bit 7 = Preview Data
  Universe (2)

DMP Layer (523 bytes):
  Flags and Length (2)
  Vector (1): VECTOR_DMP_SET_PROPERTY = 0x02
  Address Type (1): 0xA1
  First Property Address (2): 0x0000
  Address Increment (2): 0x0001
  Property Count (2): 0x0201 (513 = start code + 512 channels)
  Property Values (513): DMX start code + 512 channel values
```

### References
- [E1.31-2018 Standard](https://tsp.esta.org/tsp/documents/published_docs.php)
- [ETC sACN Library (open source)](https://github.com/ETCLabs/sACN)
- [libE131](https://github.com/hhromic/libe131)

---

## Art-Net — Art-Net 4

Widely used DMX-over-IP protocol by Artistic Licence.

### Key Details
- **Transport**: UDP broadcast/unicast
- **Port**: 6454
- **Universe addressing**: Net (7 bits) / SubNet (4 bits) / Universe (4 bits)
- **Max universes**: 32,768

### ArtDmx Packet Structure
```
ID (8): "Art-Net\0"
OpCode (2): 0x5000 (little-endian)
ProtVer (2): 14
Sequence (1): 0-255
Physical (1): Physical port
SubUni (1): Low universe byte
Net (1): High universe byte
Length (2): DMX data length (2-512, big-endian, even)
Data (512): DMX channel data
```

### ArtPoll / ArtPollReply
- Controller sends ArtPoll (OpCode 0x2000) to discover nodes
- Nodes respond with ArtPollReply (OpCode 0x2100) describing capabilities

### References
- [Art-Net Specification](https://art-net.org.uk/resources/art-net-specification/)

---

## ShowNet (Strand Proprietary)

Legacy protocol used by Strand 300/500 series consoles and ShowNet nodes.

### Key Details
- **Transport**: UDP broadcast
- **Port**: 2501 (0x09C5)
- **Encoding**: Run-Length Encoding (RLE) for DMX data
- **Capacity**: 18,432 DMX slots (36 universes)

### References
- [PyShowNet (GitHub)](https://github.com/nickvsnetworking/PyShowNet)
- [ShowNet Reverse Engineering](https://nickvsnetworking.com/reverse-engineering-dead-protocols-strand-shownet/)
- [Open Lighting Wiki](https://wiki.openlighting.org/index.php/Shownet)
