# Reduce

Reducing for the Ghoti.io Compress library. The method name is `"reduce"`; it encodes and decodes a **raw** stream with no header, trailer or checksum. It is ZIP methods 2 to 5: the method number is the compression factor plus one.

**References:** PKWARE APPNOTE section 5.2.

## The format

Two layers. The outer one is a bit stream (least significant bit first) of **follower sets**, for each byte value 255 down to 0 a six-bit size N and N bytes, then the data: each byte is coded after the byte before it, as a flag bit and `max(1, ceil(log2 N))` bits of index into that byte's set, or the flag set and eight bits, or just eight bits where the set is empty. The inner layer is in the bytes that come out: 144 introduces a repeat, or with a zero after it stands for itself. A repeat is a byte V (low `8 - factor` bits the length, the rest the top of the distance), an extra length byte if the length field is all ones, and a distance byte; it copies length + 3 bytes from distance + 1 back, zeros before the start.

## Options

| Key | Type | Default | Description |
|---|---|---|---|
| `reduce.factor` | int64 | 4 | 1 to 4. Factor f reaches 256 << f back. |
| `reduce.uncompressed_size` | uint64 | 0 | Where the decoder stops. There is no end marker, and the padding to the byte can read as a byte or two more (a follower set of one member costs two bits a byte), so a container's size should always be given. With 0 the decoder runs to the end of the input and may add a byte or two. |
| `limits.max_*` | | | Output, memory, expansion ratio (default 1024). |

## Behaviour

- **Encoder.** Buffers all the input, finds repeats of four bytes or more with hash chains, counts which bytes follow which and keeps up to 32 followers each that follow at least three times. It does not support `gcomp_encoder_flush`.
- **Readers.** None found that decode it: Info-ZIP unzip 6.00 is built without unreducing (its source tarballs carry only a stub), 7-Zip 25.01 and libarchive 3.7.4 refuse the method, and The Unarchiver 1.10.8 reports "not fully supported". What holds it to the format is real members that other projects keep as test files (written by PKZIP-era programs): `tools/legacy_zip/fetch.sh` fetches archives with reduce members at factors 1 to 4 from other projects' test data (pinned by commit and SHA-256, not committed), each unpacking to the size and CRC-32 its header records. Writing them to that standard found one thing the APPNOTE's text invites misreading: a follower index takes at least one bit, even for a set of one member. The encoder is held only to this decoder.
