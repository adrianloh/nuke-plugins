import os
import nuke

from . import loki_panel
loki_panel.register()

_MODULE_DIR = os.path.dirname(__file__)

nuke.pluginAddPath(_MODULE_DIR)

dlls = [f for f in os.listdir(_MODULE_DIR) if f.endswith('.dll')]

for dll in dlls:
    toolbar = nuke.menu("Nodes")
    try:
        # nuke.load(dll)
        nodeName = os.path.splitext(dll)[0]
        toolbar.addCommand(f'Loki/{nodeName}', f'nuke.createNode("{nodeName}")')
    except RuntimeError as e:
        print(f"Failed to load {dll}: {e}")


def _wrap_preview(text, width=40, max_lines=2):
    """Word-wrap text into at most max_lines lines, appending '...' if truncated."""
    words = text.split()
    lines = []
    current = ''
    for word in words:
        test = (current + ' ' + word).strip()
        if len(test) <= width:
            current = test
        else:
            if current:
                lines.append(current)
                if len(lines) == max_lines:
                    last = lines[-1]
                    lines[-1] = (last + '...') if len(last) + 3 <= width else (last[:width - 3] + '...')
                    return '\n'.join(lines)
            current = word
    if current:
        lines.append(current)
    return '\n'.join(lines)


def _prompt_autolabel():
    node = nuke.thisNode()
    try:
        text = node['prompt_text'].value().strip()
    except NameError:
        return None

    name = node.name()
    if text:
        return name + '\n' + _wrap_preview(text)
    else:
        return name + '\nNo Prompt'


nuke.addAutolabel(_prompt_autolabel, nodeClass='LokiEncode')
nuke.addAutolabel(_prompt_autolabel, nodeClass='LokiDecode')