class LoRaMessage {
  final String from;    // 12-char hex MAC, e.g. "AABBCCDDEEFF"
  final String text;
  final int rssi;
  final int snr;
  final DateTime timestamp;
  final bool isOwn;     // true = sent by us, false = received over LoRa

  const LoRaMessage({
    required this.from,
    required this.text,
    required this.rssi,
    required this.snr,
    required this.timestamp,
    this.isOwn = false,
  });

  /// Format the MAC address as AA:BB:CC:DD:EE:FF
  String get fromFormatted {
    if (from.length == 12) {
      return from
          .split('')
          .indexed
          .map((e) => e.$1 % 2 == 0 && e.$1 > 0 ? ':${e.$2}' : e.$2)
          .join();
    }
    return from;
  }

  /// Short display name: last 3 bytes, e.g. "DD:EE:FF"
  String get fromShort {
    if (from.length == 12) {
      final s = from.substring(6);
      return '${s.substring(0, 2)}:${s.substring(2, 4)}:${s.substring(4)}';
    }
    return from;
  }
}
