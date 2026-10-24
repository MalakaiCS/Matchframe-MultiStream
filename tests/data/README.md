# Test data

`obs-32.2.2-services.json` is OBS Studio 32.2.2's own platform database,
`plugins/rtmp-services/data/services.json` at commit ba2f32bdf791005443988a4955e963663e16b1ed
(SHA-256 b476c9f41c4f01e7e45996a793ae8b605a2bbdbfa78bfbecf1d2d35af3ea59c0), GPL-2.0-or-later like
OBS. The unit tests check that every RTMP/RTMPS server URL in it is accepted. The plugin itself
never reads this copy; at runtime it uses the database of the OBS it is loaded into.
