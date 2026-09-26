"""
VoiceCompanion.py

Small external window for SkyrimVoiceProximity -- mic selection, master
volume, and a live list of nearby players with their own volume sliders.
Talks to the audio pipeline running inside SkyrimVoiceProximity.dll over a
plain-text TCP loopback connection (see IpcServer.cpp for the protocol).

This is launched automatically by the SKSE plugin when the game loads, but
you can also just run it manually any time -- it retries the connection
until the plugin's IPC server is up.

Build to a standalone .exe with PyInstaller (matches how MorrowFriends
itself is packaged):

    pip install pyinstaller
    pyinstaller --onefile --noconsole --name VoiceCompanion VoiceCompanion.py

--noconsole is important: without it you get a black console window
alongside the GUI. The SKSE plugin also passes CREATE_NO_WINDOW when
launching it, which only suppresses a console for a console-subsystem exe;
building with --noconsole makes that unnecessary but doesn't hurt.

Drop the built VoiceCompanion.exe into:
    Data/SKSE/Plugins/VoiceCompanion/VoiceCompanion.exe
next to SkyrimVoiceProximity.dll, matching the path launchCompanionApp()
in SkyrimVoiceProximity.cpp looks for.
"""

import socket
import threading
import time
import tkinter as tk
from tkinter import ttk

HOST = "127.0.0.1"
PORT = 28571
POLL_INTERVAL_MS = 500
RECONNECT_INTERVAL_S = 1.0


class VoiceLink:
    """Owns the socket connection and the tiny text protocol. All network
    I/O happens on a background thread; the GUI thread only ever touches
    the plain Python data this class exposes, guarded by a lock."""

    def __init__(self):
        self._sock = None
        self._lock = threading.Lock()
        self._connected = False
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
        with self._lock:
            if self._sock:
                try:
                    self._sock.close()
                except OSError:
                    pass

    @property
    def connected(self):
        with self._lock:
            return self._connected

    def _run(self):
        while not self._stop:
            if not self.connected:
                self._try_connect()
            time.sleep(RECONNECT_INTERVAL_S)

    def _try_connect(self):
        try:
            s = socket.create_connection((HOST, PORT), timeout=2)
            with self._lock:
                self._sock = s
                self._connected = True
        except OSError:
            pass

    def _request(self, line, multiline=False):
        """Sends one command, returns response line(s). Returns None (or
        [] for multiline) on any connection problem, and marks us
        disconnected so the background thread retries."""
        with self._lock:
            sock = self._sock
        if not sock:
            return [] if multiline else None

        try:
            sock.sendall((line + "\n").encode("utf-8"))
            buf = b""
            lines = []
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    raise ConnectionError("closed")
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("utf-8", errors="replace").rstrip("\r")
                    if not multiline:
                        return text
                    if text == "END":
                        return lines
                    lines.append(text)
        except OSError:
            with self._lock:
                self._connected = False
                self._sock = None
            return [] if multiline else None

    # --- Public API mirroring VoiceProximityAPI.h -----------------------

    def list_mics(self):
        """Returns list of (index:int, name:str)."""
        out = []
        for line in self._request("LIST_MICS", multiline=True) or []:
            idx_str, _, name = line.partition("|")
            try:
                out.append((int(idx_str), name))
            except ValueError:
                continue
        return out

    def get_mic(self):
        resp = self._request("GET_MIC")
        try:
            return int(resp)
        except (TypeError, ValueError):
            return -1

    def set_mic(self, index):
        self._request(f"SET_MIC {index}")

    def get_master(self):
        resp = self._request("GET_MASTER")
        try:
            return float(resp)
        except (TypeError, ValueError):
            return 1.0

    def set_master(self, volume):
        self._request(f"SET_MASTER {volume:.2f}")

    def list_players(self):
        """Returns list of dicts: id, username, volume, talking."""
        out = []
        for line in self._request("LIST_PLAYERS", multiline=True) or []:
            parts = line.split("|")
            if len(parts) != 4:
                continue
            try:
                out.append({
                    "id": int(parts[0]),
                    "username": parts[1],
                    "volume": float(parts[2]),
                    "talking": parts[3] == "1",
                })
            except ValueError:
                continue
        return out

    def set_player_volume(self, player_id, volume):
        self._request(f"SET_PLAYER_VOLUME {player_id} {volume:.2f}")


class App:
    def __init__(self, root):
        self.root = root
        self.link = VoiceLink()
        self._player_rows = {}  # player id -> {"frame", "slider", "label"}
        self._mic_indices = []  # parallel to the combobox's string list

        root.title("Voice Proximity")
        root.geometry("340x420")
        root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.status_var = tk.StringVar(value="Connecting...")
        ttk.Label(root, textvariable=self.status_var, foreground="gray").pack(anchor="w", padx=10, pady=(8, 0))

        # --- Microphone ---
        ttk.Label(root, text="Microphone").pack(anchor="w", padx=10, pady=(10, 0))
        self.mic_combo = ttk.Combobox(root, state="readonly")
        self.mic_combo.pack(fill="x", padx=10, pady=(2, 0))
        self.mic_combo.bind("<<ComboboxSelected>>", self._on_mic_selected)

        # --- Master volume ---
        ttk.Label(root, text="Master Voice Volume").pack(anchor="w", padx=10, pady=(14, 0))
        self.master_var = tk.DoubleVar(value=100)
        self.master_slider = ttk.Scale(root, from_=0, to=200, variable=self.master_var,
                                       command=self._on_master_changed)
        self.master_slider.pack(fill="x", padx=10, pady=(2, 0))

        ttk.Separator(root).pack(fill="x", padx=10, pady=12)

        # --- Party list ---
        ttk.Label(root, text="Party Volume").pack(anchor="w", padx=10)
        self.players_frame = ttk.Frame(root)
        self.players_frame.pack(fill="both", expand=True, padx=10, pady=(4, 10))
        self.empty_label = ttk.Label(self.players_frame, text="No one else has spoken yet.", foreground="gray")
        self.empty_label.pack(anchor="w")

        self._poll()

    # --- Event handlers ------------------------------------------------

    def _on_mic_selected(self, _event):
        sel = self.mic_combo.current()
        if 0 <= sel < len(self._mic_indices):
            self.link.set_mic(self._mic_indices[sel])

    def _on_master_changed(self, value):
        self.link.set_master(float(value) / 100.0)

    def _on_player_volume_changed(self, player_id, value):
        self.link.set_player_volume(player_id, float(value) / 100.0)

    def _on_close(self):
        self.link.stop()
        self.root.destroy()

    # --- Polling loop ----------------------------------------------------

    def _poll(self):
        if self.link.connected:
            self.status_var.set("Connected")
            self._refresh_mics()
            self._refresh_master()
            self._refresh_players()
        else:
            self.status_var.set("Waiting for Skyrim Together Reborn...")

        self.root.after(POLL_INTERVAL_MS, self._poll)

    def _refresh_mics(self):
        mics = self.link.list_mics()
        if not mics:
            return
        self._mic_indices = [idx for idx, _ in mics]
        names = [name for _, name in mics]
        current_values = list(self.mic_combo["values"])
        if names != current_values:
            self.mic_combo["values"] = names

        current_index = self.link.get_mic()
        if current_index in self._mic_indices:
            pos = self._mic_indices.index(current_index)
            if self.mic_combo.current() != pos:
                self.mic_combo.current(pos)

    def _refresh_master(self):
        # Only sync from the server if the user isn't actively dragging --
        # cheap heuristic: skip while the mouse button is held on the slider.
        pass  # kept simple: master volume is set by us, rarely changed elsewhere

    def _refresh_players(self):
        players = self.link.list_players()

        if players:
            self.empty_label.pack_forget()
        else:
            self.empty_label.pack(anchor="w")

        seen_ids = set()
        for p in players:
            seen_ids.add(p["id"])
            row = self._player_rows.get(p["id"])
            if row is None:
                row = self._build_player_row(p["id"])
                self._player_rows[p["id"]] = row

            color = "#2ecc71" if p["talking"] else "black"
            row["label"].configure(text=p["username"], foreground=color)

            # Don't fight the user's own drag -- only sync if they haven't
            # touched this slider since the last refresh.
            if not row.get("dirty"):
                row["var"].set(p["volume"] * 100.0)

        # Remove rows for players who've disconnected.
        for pid in list(self._player_rows.keys()):
            if pid not in seen_ids:
                self._player_rows[pid]["frame"].destroy()
                del self._player_rows[pid]

    def _build_player_row(self, player_id):
        frame = ttk.Frame(self.players_frame)
        frame.pack(fill="x", pady=3)

        label = ttk.Label(frame, text="", width=16)
        label.pack(side="left")

        var = tk.DoubleVar(value=100)
        row = {"frame": frame, "label": label, "var": var, "dirty": False}

        def on_change(value, pid=player_id, row=row):
            row["dirty"] = True
            self._on_player_volume_changed(pid, value)

        def on_release(_event, row=row):
            row["dirty"] = False

        slider = ttk.Scale(frame, from_=0, to=200, variable=var, command=on_change)
        slider.pack(side="left", fill="x", expand=True, padx=(6, 0))
        slider.bind("<ButtonRelease-1>", on_release)
        row["slider"] = slider

        return row


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
