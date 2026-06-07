/*
 * TrailText
 * Text when networks fail.
 *
 * Copyright (c) 2026 Bruno Keymolen
 *
 * This work is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 4.0 International License.
 *
 * You are free to share and adapt this work for non-commercial purposes,
 * provided that appropriate credit is given and any derivative works are
 * distributed under the same license.
 *
 * License: CC BY-NC-SA 4.0
 * See: https://creativecommons.org/licenses/by-nc-sa/4.0/
 */

class LoRaMessage {
  final String from; // 12-char hex MAC, e.g. "AABBCCDDEEFF"
  final String text;
  final int rssi;
  final int snr;
  final DateTime timestamp;
  final bool isOwn; // true = sent by us, false = received over LoRa
  final String? messageId;
  final bool isAcked;
  final DateTime? ackedAt;
  final double? latitude;
  final double? longitude;

  const LoRaMessage({
    required this.from,
    required this.text,
    required this.rssi,
    required this.snr,
    required this.timestamp,
    this.isOwn = false,
    this.messageId,
    this.isAcked = false,
    this.ackedAt,
    this.latitude,
    this.longitude,
  });

  bool get hasCoordinates => latitude != null && longitude != null;

  LoRaMessage copyWith({
    String? from,
    String? text,
    int? rssi,
    int? snr,
    DateTime? timestamp,
    bool? isOwn,
    String? messageId,
    bool? isAcked,
    DateTime? ackedAt,
    double? latitude,
    double? longitude,
  }) {
    return LoRaMessage(
      from: from ?? this.from,
      text: text ?? this.text,
      rssi: rssi ?? this.rssi,
      snr: snr ?? this.snr,
      timestamp: timestamp ?? this.timestamp,
      isOwn: isOwn ?? this.isOwn,
      messageId: messageId ?? this.messageId,
      isAcked: isAcked ?? this.isAcked,
      ackedAt: ackedAt ?? this.ackedAt,
      latitude: latitude ?? this.latitude,
      longitude: longitude ?? this.longitude,
    );
  }

  Map<String, Object?> toJson() {
    return {
      'from': from,
      'text': text,
      'rssi': rssi,
      'snr': snr,
      'timestamp': timestamp.toIso8601String(),
      'isOwn': isOwn,
      'messageId': messageId,
      'isAcked': isAcked,
      'ackedAt': ackedAt?.toIso8601String(),
      'latitude': latitude,
      'longitude': longitude,
    };
  }

  factory LoRaMessage.fromJson(Map<String, dynamic> json) {
    return LoRaMessage(
      from: json['from'] as String? ?? '',
      text: json['text'] as String? ?? '',
      rssi: (json['rssi'] as num?)?.toInt() ?? 0,
      snr: (json['snr'] as num?)?.toInt() ?? 0,
      timestamp: DateTime.tryParse(json['timestamp'] as String? ?? '') ??
          DateTime.now(),
      isOwn: json['isOwn'] as bool? ?? false,
      messageId: json['messageId'] as String?,
      isAcked: json['isAcked'] as bool? ?? false,
      ackedAt: json['ackedAt'] == null
          ? null
          : DateTime.tryParse(json['ackedAt'] as String),
      latitude: (json['latitude'] as num?)?.toDouble(),
      longitude: (json['longitude'] as num?)?.toDouble(),
    );
  }

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
