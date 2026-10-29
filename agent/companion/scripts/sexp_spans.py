"""Lossless KiCad S-expression spans for deliberate schematic transformations."""
import re
TOKEN=re.compile(r'\s+|"(?:\\.|[^"\\])*"|[()]|[^\s()]+')
class Node:
    def __init__(self,start): self.start=start; self.end=None; self.items=[]
    @property
    def key(self): return self.items[0] if self.items else ''
    def children(self,key): return [x for x in self.items if isinstance(x,Node) and x.key==key]
    def child(self,key): return next(iter(self.children(key)),None)
def parse(text):
    stack=[]; root=None
    for m in TOKEN.finditer(text):
        s=m.group()
        if s.isspace(): continue
        if s=='(':
            n=Node(m.start())
            if stack: stack[-1].items.append(n)
            else: root=n
            stack.append(n)
        elif s==')': stack.pop().end=m.end()
        else: stack[-1].items.append(s[1:-1] if s.startswith('"') else s)
    assert root is not None and not stack
    return root
def apply(text,edits):
    last=len(text)+1
    for start,end,replacement in sorted(edits,reverse=True):
        assert end<=last, 'Overlapping edits'
        text=text[:start]+replacement+text[end:];last=start
    return text
