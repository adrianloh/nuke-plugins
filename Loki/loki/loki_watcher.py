"""
loki_watcher.py

A floating Qt panel that watches a Loki node's "text" knob and displays
its current value, polling on a timer.

Usage from menu.py:
    import loki_watcher
    nuke.menu("Nuke").findItem("Loki").addCommand(
        "Watcher", "loki_watcher.show_watcher()")
"""

import nuke
from PySide2 import QtWidgets, QtCore, QtGui


# Module-level reference so the window survives the function scope
# and you don't get "window closes immediately" garbage-collection issues.
_watcher_instance = None


class LokiWatcher(QtWidgets.QWidget):

    def __init__(self, parent=None):
        super(LokiWatcher, self).__init__(parent)

        self.setWindowTitle("Loki Watcher")
        self.setWindowFlags(QtCore.Qt.Window | QtCore.Qt.WindowStaysOnTopHint)
        self.resize(500, 300)

        # --- Layout ---

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        # Top row: node name input + interval
        top_row = QtWidgets.QHBoxLayout()
        top_row.setSpacing(6)

        top_row.addWidget(QtWidgets.QLabel("Node:"))

        self.node_input = QtWidgets.QLineEdit()
        self.node_input.setPlaceholderText("LokiDecode1")
        self.node_input.returnPressed.connect(self._on_node_changed)
        top_row.addWidget(self.node_input, 1)

        top_row.addWidget(QtWidgets.QLabel("Knob:"))
        self.knob_input = QtWidgets.QLineEdit("text")
        self.knob_input.setFixedWidth(80)
        self.knob_input.returnPressed.connect(self._on_node_changed)
        top_row.addWidget(self.knob_input)

        top_row.addWidget(QtWidgets.QLabel("Poll (ms):"))
        self.interval_input = QtWidgets.QSpinBox()
        self.interval_input.setRange(50, 10000)
        self.interval_input.setSingleStep(50)
        self.interval_input.setValue(250)
        self.interval_input.valueChanged.connect(self._on_interval_changed)
        top_row.addWidget(self.interval_input)

        layout.addLayout(top_row)

        # Status line
        self.status_label = QtWidgets.QLabel("Not watching")
        self.status_label.setStyleSheet("color: #888;")
        layout.addWidget(self.status_label)

        # Text display
        self.text_display = QtWidgets.QPlainTextEdit()
        self.text_display.setReadOnly(True)
        font = QtGui.QFont("Consolas, Menlo, monospace")
        font.setStyleHint(QtGui.QFont.Monospace)
        self.text_display.setFont(font)
        layout.addWidget(self.text_display, 1)

        # Bottom row: char count
        self.count_label = QtWidgets.QLabel("0 chars")
        self.count_label.setStyleSheet("color: #888;")
        layout.addWidget(self.count_label)

        # --- State ---

        self._last_text = None
        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(self.interval_input.value())

    # --- Slots ---

    def _on_node_changed(self):
        self._last_text = None  # force refresh
        self._poll()

    def _on_interval_changed(self, value):
        self._timer.setInterval(value)

    def _poll(self):
        node_name = self.node_input.text().strip()
        knob_name = self.knob_input.text().strip() or "text"

        if not node_name:
            self._set_status("Enter a node name above")
            return

        node = nuke.toNode(node_name)
        if node is None:
            self._set_status("Node '%s' not found" % node_name, error=True)
            return

        knob = node.knob(knob_name)
        if knob is None:
            self._set_status(
                "Node '%s' has no knob '%s'" % (node_name, knob_name),
                error=True)
            return

        try:
            text = knob.getValue() if hasattr(knob, 'getValue') else str(knob.value())
            # For String_knob/Multiline_String_knob, .value() returns the string
            if hasattr(knob, 'value'):
                v = knob.value()
                if isinstance(v, str):
                    text = v
        except Exception as e:
            self._set_status("Error reading knob: %s" % e, error=True)
            return

        # Only update the widget if the text actually changed — avoids
        # constant repaints that would blow away the user's cursor/scroll.
        if text != self._last_text:
            self._last_text = text
            self.text_display.setPlainText(text)
            self.count_label.setText("%d chars" % len(text))
            self._set_status(
                "Watching %s.%s" % (node_name, knob_name))

    def _set_status(self, msg, error=False):
        color = "#e66" if error else "#8a8"
        self.status_label.setStyleSheet("color: %s;" % color)
        self.status_label.setText(msg)

    def closeEvent(self, event):
        self._timer.stop()
        global _watcher_instance
        _watcher_instance = None
        super(LokiWatcher, self).closeEvent(event)


def show_watcher():
    """Show the Loki Watcher window. Raises an existing one if already open."""
    global _watcher_instance
    if _watcher_instance is not None:
        try:
            _watcher_instance.raise_()
            _watcher_instance.activateWindow()
            return _watcher_instance
        except RuntimeError:
            # Window was destroyed (e.g. C++ side deleted) — fall through
            _watcher_instance = None

    _watcher_instance = LokiWatcher()

    # If a node is selected, pre-fill the name
    try:
        sel = nuke.selectedNode()
        if sel is not None:
            _watcher_instance.node_input.setText(sel.name())
    except ValueError:
        pass

    _watcher_instance.show()
    return _watcher_instance
