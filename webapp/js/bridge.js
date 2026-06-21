(function(){
if (window.__androidBridgeLoaded) return;
window.__androidBridgeLoaded = true;
if (!window.NSBridge) return;
window.__bridge = window.__bridge || {};
if (typeof NSBridge.onBinary === 'function') {
    window.__bridge.send = function(data) {
        if (data instanceof ArrayBuffer) {
            var u8 = new Uint8Array(data);
            var arr = [];
            for (var i = 0; i < u8.length; i++) arr.push(u8[i]);
            NSBridge.onBinary(JSON.stringify(arr));
        }
    };
}
window.WebSocket = function(url, protocols) {
    this.readyState = 0;
    this.binaryType = 'arraybuffer';
    this.onopen = null; this.onclose = null; this.onerror = null; this.onmessage = null;
    this.send = function(data) { window.__bridge.send(data); };
    this.close = function() { if (NSBridge.onClose) NSBridge.onClose(); else if (window.__bridge && window.__bridge.close) window.__bridge.close(); };
    if (window.__bridge.connect) window.__bridge.connect(url);
    setTimeout(function() {
        this.readyState = 1;
        if (NSBridge.onOpen) NSBridge.onOpen();
        if (this.onopen) this.onopen();
    }.bind(this), 0);
};
window.WebSocket.CONNECTING = 0;
window.WebSocket.OPEN = 1;
window.WebSocket.CLOSING = 2;
window.WebSocket.CLOSED = 3;
})();
