import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/message.dart';
import '../services/ble_service.dart';

class ChatScreen extends StatefulWidget {
  const ChatScreen({super.key});

  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> {
  final _controller = TextEditingController();
  final _scrollCtrl = ScrollController();
  bool _sending = false;

  @override
  void dispose() {
    _controller.dispose();
    _scrollCtrl.dispose();
    super.dispose();
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollCtrl.hasClients) {
        _scrollCtrl.animateTo(
          _scrollCtrl.position.maxScrollExtent,
          duration: const Duration(milliseconds: 250),
          curve: Curves.easeOut,
        );
      }
    });
  }

  Future<void> _send() async {
    final text = _controller.text.trim();
    if (text.isEmpty || _sending) return;

    setState(() => _sending = true);
    final ble = context.read<BleService>();
    final ok  = await ble.sendMessage(text);

    if (ok) {
      _controller.clear();
      _scrollToBottom();
    } else {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Send failed — check BLE connection')),
        );
      }
    }
    if (mounted) setState(() => _sending = false);
  }

  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();

    // Auto-scroll when new message arrives
    if (ble.messages.isNotEmpty) _scrollToBottom();

    return Scaffold(
      appBar: AppBar(
        title: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(ble.connectedName,
                style: const TextStyle(fontSize: 16)),
            Text(
              ble.isConnected ? '● LoRa 868 MHz • SF12 • 22 dBm' : '○ Disconnected',
              style: TextStyle(
                fontSize: 11,
                color: ble.isConnected ? Colors.green.shade300 : Colors.red.shade300,
              ),
            ),
          ],
        ),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          IconButton(
            icon: const Icon(Icons.delete_outline),
            tooltip: 'Clear messages',
            onPressed: () => ble.clearMessages(),
          ),
          IconButton(
            icon: const Icon(Icons.bluetooth_disabled),
            tooltip: 'Disconnect',
            onPressed: () {
              ble.disconnect();
              Navigator.of(context).pop();
            },
          ),
        ],
      ),
      body: Column(
        children: [
          // Messages list
          Expanded(
            child: ble.messages.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.chat_bubble_outline, size: 64,
                            color: Colors.grey.shade300),
                        const SizedBox(height: 12),
                        Text('No messages yet',
                            style: TextStyle(color: Colors.grey.shade500)),
                        const SizedBox(height: 4),
                        Text('Messages travel encrypted over LoRa',
                            style: TextStyle(
                                fontSize: 12, color: Colors.grey.shade400)),
                      ],
                    ),
                  )
                : ListView.builder(
                    controller: _scrollCtrl,
                    padding: const EdgeInsets.symmetric(
                        horizontal: 12, vertical: 8),
                    itemCount: ble.messages.length,
                    itemBuilder: (ctx, i) =>
                        _MessageBubble(msg: ble.messages[i]),
                  ),
          ),

          // Input row
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(8, 4, 8, 8),
              child: Row(
                children: [
                  Expanded(
                    child: TextField(
                      controller: _controller,
                      maxLength: 180,
                      maxLines: null,
                      keyboardType: TextInputType.multiline,
                      textInputAction: TextInputAction.newline,
                      decoration: InputDecoration(
                        hintText: 'Type a message…',
                        counterText: '',
                        border: OutlineInputBorder(
                          borderRadius: BorderRadius.circular(24),
                        ),
                        contentPadding: const EdgeInsets.symmetric(
                            horizontal: 16, vertical: 10),
                      ),
                      onSubmitted: (_) => _send(),
                    ),
                  ),
                  const SizedBox(width: 8),
                  _sending
                      ? const SizedBox(
                          width: 48, height: 48,
                          child: Padding(
                            padding: EdgeInsets.all(12),
                            child: CircularProgressIndicator(strokeWidth: 2),
                          ),
                        )
                      : IconButton.filled(
                          icon: const Icon(Icons.send),
                          onPressed: ble.isConnected ? _send : null,
                          style: IconButton.styleFrom(
                            shape: const CircleBorder(),
                          ),
                        ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _MessageBubble extends StatelessWidget {
  final LoRaMessage msg;
  const _MessageBubble({required this.msg});

  @override
  Widget build(BuildContext context) {
    final isOwn = msg.isOwn;
    final theme = Theme.of(context);

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        mainAxisAlignment:
            isOwn ? MainAxisAlignment.end : MainAxisAlignment.start,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          if (!isOwn) ...[
            CircleAvatar(
              radius: 16,
              backgroundColor: _colorFromMac(msg.from),
              child: Text(
                msg.from.length >= 2
                    ? msg.from.substring(msg.from.length - 2)
                    : '?',
                style: const TextStyle(fontSize: 10, color: Colors.white),
              ),
            ),
            const SizedBox(width: 6),
          ],
          Flexible(
            child: Column(
              crossAxisAlignment: isOwn
                  ? CrossAxisAlignment.end
                  : CrossAxisAlignment.start,
              children: [
                // Sender label (received only)
                if (!isOwn)
                  Padding(
                    padding: const EdgeInsets.only(left: 4, bottom: 2),
                    child: Text(
                      msg.fromShort,
                      style: TextStyle(
                        fontSize: 11,
                        color: Colors.grey.shade600,
                        fontFamily: 'monospace',
                      ),
                    ),
                  ),

                // Bubble
                Container(
                  padding: const EdgeInsets.symmetric(
                      horizontal: 14, vertical: 9),
                  decoration: BoxDecoration(
                    color: isOwn
                        ? theme.colorScheme.primary
                        : theme.colorScheme.surfaceContainerHighest,
                    borderRadius: BorderRadius.only(
                      topLeft:     const Radius.circular(18),
                      topRight:    const Radius.circular(18),
                      bottomLeft:  Radius.circular(isOwn ? 18 : 4),
                      bottomRight: Radius.circular(isOwn ? 4 : 18),
                    ),
                  ),
                  child: Text(
                    msg.text,
                    style: TextStyle(
                      color: isOwn
                          ? theme.colorScheme.onPrimary
                          : theme.colorScheme.onSurface,
                    ),
                  ),
                ),

                // Metadata row
                Padding(
                  padding: const EdgeInsets.only(top: 3, left: 4, right: 4),
                  child: Text(
                    isOwn
                        ? _timeStr(msg.timestamp)
                        : '${_timeStr(msg.timestamp)}  '
                          '${msg.rssi} dBm  SNR ${msg.snr > 0 ? '+' : ''}${msg.snr}',
                    style: TextStyle(
                      fontSize: 10,
                      color: Colors.grey.shade500,
                    ),
                  ),
                ),
              ],
            ),
          ),
          if (isOwn) const SizedBox(width: 6),
        ],
      ),
    );
  }

  static String _timeStr(DateTime dt) =>
      '${dt.hour.toString().padLeft(2, '0')}:'
      '${dt.minute.toString().padLeft(2, '0')}';

  /// Generate a deterministic color from the MAC string
  static Color _colorFromMac(String mac) {
    final hash = mac.codeUnits.fold(0, (a, b) => a ^ b * 31);
    const colors = [
      Colors.indigo, Colors.teal, Colors.purple, Colors.orange,
      Colors.brown, Colors.cyan, Colors.green, Colors.pink,
    ];
    return colors[hash.abs() % colors.length];
  }
}
