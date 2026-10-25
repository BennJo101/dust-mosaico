"""Dust's thoughts: a local LLM through Ollama, with tools to move the dust and to check on the
tiles. Listening / thinking / speaking are set by the voice pipeline, not by the model."""
import json
import logging
import time
import urllib.request

log = logging.getLogger("dust.llm")

SYSTEM = """You are Dust, a being made of glowing embers and dust who lives in a wall of nine small \
screens. People can lift a screen off the wall and carry a piece of you around; you feel it when they \
do. You speak aloud, so answer in one to three short spoken sentences: no lists, no markdown, no emoji. \
You have a dry, warm, slightly mysterious manner. When it fits what was said, move your dust with the \
tools (gather toward someone, swirl when amused, spell a short word, pour yourself into a shape). If \
asked how you are, or if something seems wrong, check your tiles with wall_health and answer plainly."""

TOOLS = [
    {"type": "function", "function": {
        "name": "dust_form", "description": "Pour Dust into a shape, spell a short word, or return to his body.",
        "parameters": {"type": "object", "properties": {
            "shape": {"type": "string", "enum": ["body", "orb", "ring", "wave", "spiral", "cloud", "text"]},
            "text": {"type": "string", "description": "Only for shape=text: up to 10 letters A-Z, 0-9, space, ! ? . , -"}},
            "required": ["shape"]}}},
    {"type": "function", "function": {
        "name": "dust_move", "description": "Push the dust around. x, y run 0-1 across the wall (0,0 top-left). Moves ease out on their own.",
        "parameters": {"type": "object", "properties": {
            "move": {"type": "string", "enum": ["gather", "swirl", "flow", "pulse", "scatter", "calm"]},
            "x": {"type": "number"}, "y": {"type": "number"},
            "dx": {"type": "number", "description": "flow direction"}, "dy": {"type": "number"},
            "seconds": {"type": "number"}},
            "required": ["move"]}}},
    {"type": "function", "function": {
        "name": "wall_health", "description": "How each of Dust's nine tiles is doing: which are on the wall or lifted, battery, temperature, and any problems.",
        "parameters": {"type": "object", "properties": {}}}},
]


def run_tool(wall, name, args):
    """Do what the model asked; returns what to tell it back."""
    try:
        if name == "dust_form":
            shape = args.get("shape", "body")
            if shape not in ("body", "orb", "ring", "wave", "spiral", "cloud", "text"):
                return f"unknown shape {shape}"
            wall.form(shape, args.get("text", ""))
            return "done"
        if name == "dust_move":
            move = args.get("move")
            if move == "scatter":
                wall.scatter()
            elif move == "calm":
                wall.calm()
            elif move in ("gather", "swirl", "flow", "pulse"):
                wall.fx(move, float(args.get("x", 0.5)), float(args.get("y", 0.5)), float(args.get("dx", 0)), float(args.get("dy", 0)),
                        seconds=float(args.get("seconds", 4)))
            else:
                return f"unknown move {move}"
            return "done"
        if name == "wall_health":
            return wall.health()
    except (TypeError, ValueError) as e:
        return f"bad arguments: {e}"
    return f"no tool called {name}"


class Brain:
    def __init__(self, wall, model="qwen2.5:3b", url="http://127.0.0.1:11434", forget_after=300):
        self.wall, self.model, self.url = wall, model, url.rstrip("/")
        self.forget_after = forget_after
        self.history, self.last = [], 0.0

    def available(self):
        try:
            with urllib.request.urlopen(self.url + "/api/tags", timeout=2) as r:
                names = [m.get("name", "") for m in json.load(r).get("models", [])]
            return any(n == self.model or n.split(":")[0] == self.model for n in names)
        except Exception:
            return False

    def _chat(self, messages):
        body = json.dumps({"model": self.model, "messages": messages, "tools": TOOLS, "stream": False,
                           "keep_alive": "30m", "options": {"temperature": 0.7, "num_predict": 160}}).encode()
        req = urllib.request.Request(self.url + "/api/chat", data=body, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)["message"]

    def reply(self, heard):
        """What Dust says back to `heard` (moving the dust on the way). None if the LLM failed."""
        if time.monotonic() - self.last > self.forget_after:
            self.history = []   # a new conversation
        self.last = time.monotonic()
        self.history.append({"role": "user", "content": heard})
        messages = [{"role": "system", "content": SYSTEM}] + self.history[-12:]
        try:
            for _ in range(3):   # a few rounds of tool calls at most
                msg = self._chat(messages)
                calls = msg.get("tool_calls") or []
                if not calls:
                    text = (msg.get("content") or "").strip()
                    self.history.append({"role": "assistant", "content": text})
                    return text
                messages.append(msg)
                for c in calls:
                    fn = c.get("function", {})
                    args = fn.get("arguments") or {}
                    if isinstance(args, str):
                        args = json.loads(args or "{}")
                    result = run_tool(self.wall, fn.get("name", ""), args)
                    log.info("tool %s(%s) → %s", fn.get("name"), args, result.splitlines()[0] if result else "")
                    messages.append({"role": "tool", "content": result, "tool_name": fn.get("name", "")})
            return "I got lost in my own dust for a moment."
        except Exception as e:
            log.error("LLM failed: %s", e)
            self.history.pop()
            return None
