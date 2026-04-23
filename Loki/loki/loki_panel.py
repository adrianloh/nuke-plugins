
import nuke
from PySide2 import QtWidgets, QtCore, QtGui

POLL_INTERVAL_MS = 250

class PromptWatcherWidget(QtWidgets.QWidget):
    def __init__(self, node):
        super(PromptWatcherWidget, self).__init__()
        self._node = node
        self._last_text = None

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)

        header = QtWidgets.QLabel("prompt_text")
        header.setStyleSheet("color: #aaa; font-weight: bold;")
        layout.addWidget(header)

        self._display = QtWidgets.QPlainTextEdit()
        self._display.setReadOnly(True)
        self._display.setMinimumHeight(120)
        font = QtGui.QFont("Consolas, Menlo, monospace")
        font.setStyleHint(QtGui.QFont.Monospace)
        self._display.setFont(font)
        layout.addWidget(self._display)

        self._count = QtWidgets.QLabel("0 chars")
        self._count.setStyleSheet("color: #888;")
        layout.addWidget(self._count)

        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(POLL_INTERVAL_MS)

        self._poll()

    def _poll(self):
        # GUARD 1: If obscured or tab-switched, don't waste CPU
        if not self.isVisible():
            return

        # GUARD 2: Check if node still exists to prevent C++ memory access crashes
        if not nuke.exists(self._node.fullName()):
            self._timer.stop()
            return
            
        try:
            k = self._node.knob("prompt_text")
            if k is None:
                return
            text = k.value()
        except Exception:
            return

        if text != self._last_text:
            self._last_text = text
            self._display.setPlainText(text if text is not None else "")
            self._count.setText("%d chars" % len(text or ""))

    def hideEvent(self, event):
        # Ensure the timer stops when the panel is closed
        self._timer.stop()
        super(PromptWatcherWidget, self).hideEvent(event)

class PromptWatcher(object):
    def __init__(self, node):
        self._node = node
        self._widget = None

    def makeUI(self):
        self._widget = PromptWatcherWidget(self._node)
        return self._widget

    def updateValue(self):
        pass

def _make_watcher(node):
    return PromptWatcher(node)

def register():
    nuke._loki_make_watcher = _make_watcher
    nuke.addOnUserCreate(_on_user_create, nodeClass="LokiDecode")
    nuke.addOnScriptLoad(_on_script_load)

def _on_user_create():
    node = nuke.thisNode()
    if node is None or node.knob("watcher") is not None:
        return
    _add_watcher_knob(node)

def _on_script_load():
    for n in nuke.allNodes("LokiDecode"):
        if n.knob("watcher") is None:
            _add_watcher_knob(n)

def _add_watcher_knob(node):
    cmd = "nuke._loki_make_watcher(nuke.thisNode())"
    k = nuke.PyCustom_Knob("watcher", "", cmd)
    node.addKnob(k)
