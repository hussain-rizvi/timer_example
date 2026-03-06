"""
Race Timer BLE GUI

Connects to the nRF52840-based Race Timer over BLE and provides controls
to start races, select modes, and view individual lane times in real time.
"""

import asyncio
import struct
import threading
import time
from enum import IntEnum

import customtkinter as ctk
from bleak import BleakClient, BleakScanner

# ── BLE UUIDs ──
SERVICE_UUID  = "00001000-7261-6365-7469-6d6572303031"
CMD_UUID      = "00001001-7261-6365-7469-6d6572303031"
EVT_UUID      = "00001002-7261-6365-7469-6d6572303031"
STATUS_UUID   = "00001003-7261-6365-7469-6d6572303031"

DEVICE_NAME = "RaceTimer"

# ── Command IDs ──
CMD_START_RACE = 0x01
CMD_NEW_RACE   = 0x02
CMD_RESET      = 0x03
CMD_PING       = 0x04
CMD_GET_STATUS = 0x05
CMD_SET_MODE   = 0x06

# ── Event IDs ──
EVT_START_ACK    = 0x10
EVT_STOP_EVENT   = 0x11
EVT_RACE_COMPLETE = 0x12
EVT_STATUS       = 0x13
EVT_PONG         = 0x14
EVT_ERROR        = 0x1F

# ── Race Modes ──
RACE_MODE_4 = 0x01
RACE_MODE_1 = 0x02

# ── Race States ──
class RaceState(IntEnum):
    DISCONNECTED = 0
    IDLE = 1
    CONFIGURED = 2
    RUNNING = 3
    FINISHED = 4
    ERROR = 5


def format_time(ms):
    """Format milliseconds as MM:SS.mmm"""
    if ms is None:
        return "--:--.---"
    minutes = ms // 60000
    seconds = (ms % 60000) // 1000
    millis = ms % 1000
    return f"{minutes:02d}:{seconds:02d}.{millis:03d}"


LANE_COLORS = {
    1: "#e74c3c",  # red
    2: "#2ecc71",  # green
    3: "#3498db",  # blue
    4: "#f39c12",  # orange
}


class RaceTimerApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("Race Timer")
        self.geometry("520x720")
        self.resizable(False, False)
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")

        self.client: BleakClient | None = None
        self.connected = False
        self.race_running = False
        self.race_start_local = 0.0
        self.lane_times: dict[int, int | None] = {1: None, 2: None, 3: None, 4: None}
        self.winner: int | None = None
        self.winner_time: int | None = None
        self.finish_order: list[int] = []

        self._ble_loop: asyncio.AbstractEventLoop | None = None
        self._ble_thread: threading.Thread | None = None

        self._build_ui()
        self._start_ble_thread()
        self._tick_elapsed()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ── UI Construction ──

    def _build_ui(self):
        pad = {"padx": 16, "pady": (8, 0)}

        # Title
        ctk.CTkLabel(
            self, text="Race Timer", font=ctk.CTkFont(size=28, weight="bold")
        ).pack(pady=(18, 4))

        # Connection frame
        conn_frame = ctk.CTkFrame(self)
        conn_frame.pack(fill="x", **pad)

        self.btn_connect = ctk.CTkButton(
            conn_frame, text="Scan & Connect", command=self._on_connect, width=160
        )
        self.btn_connect.pack(side="left", padx=12, pady=12)

        self.lbl_status = ctk.CTkLabel(
            conn_frame, text="Disconnected", text_color="#e74c3c",
            font=ctk.CTkFont(size=14, weight="bold"),
        )
        self.lbl_status.pack(side="left", padx=12)

        # Mode + controls frame
        ctrl_frame = ctk.CTkFrame(self)
        ctrl_frame.pack(fill="x", **pad)

        ctk.CTkLabel(ctrl_frame, text="Mode:", font=ctk.CTkFont(size=14)).pack(
            side="left", padx=(12, 4), pady=12
        )
        self.mode_var = ctk.StringVar(value="4 Contestants")
        self.mode_menu = ctk.CTkOptionMenu(
            ctrl_frame,
            variable=self.mode_var,
            values=["4 Contestants", "1 Contestant"],
            width=150,
        )
        self.mode_menu.pack(side="left", padx=4, pady=12)

        self.btn_start = ctk.CTkButton(
            ctrl_frame, text="Start Race", command=self._on_start, width=110,
            fg_color="#27ae60", hover_color="#219a52",
        )
        self.btn_start.pack(side="left", padx=8, pady=12)

        self.btn_new = ctk.CTkButton(
            ctrl_frame, text="New Race", command=self._on_new_race, width=100,
            fg_color="#7f8c8d", hover_color="#636e72",
        )
        self.btn_new.pack(side="left", padx=4, pady=12)

        # Elapsed time
        elapsed_frame = ctk.CTkFrame(self)
        elapsed_frame.pack(fill="x", **pad)

        ctk.CTkLabel(
            elapsed_frame, text="Elapsed", font=ctk.CTkFont(size=14)
        ).pack(side="left", padx=12, pady=14)

        self.lbl_elapsed = ctk.CTkLabel(
            elapsed_frame, text="00:00.000",
            font=ctk.CTkFont(family="Consolas", size=36, weight="bold"),
        )
        self.lbl_elapsed.pack(side="left", padx=12, pady=14)

        # Lane results
        self.lane_frames: dict[int, ctk.CTkFrame] = {}
        self.lane_time_labels: dict[int, ctk.CTkLabel] = {}
        self.lane_pos_labels: dict[int, ctk.CTkLabel] = {}

        for lane in range(1, 5):
            frame = ctk.CTkFrame(self, border_width=2, border_color=LANE_COLORS[lane])
            frame.pack(fill="x", **pad)
            self.lane_frames[lane] = frame

            ctk.CTkLabel(
                frame,
                text=f"  Lane {lane}",
                font=ctk.CTkFont(size=16, weight="bold"),
                text_color=LANE_COLORS[lane],
            ).pack(side="left", padx=8, pady=12)

            pos_lbl = ctk.CTkLabel(
                frame, text="", font=ctk.CTkFont(size=14, weight="bold"), width=40
            )
            pos_lbl.pack(side="right", padx=(0, 12), pady=12)
            self.lane_pos_labels[lane] = pos_lbl

            time_lbl = ctk.CTkLabel(
                frame, text="--:--.---",
                font=ctk.CTkFont(family="Consolas", size=24, weight="bold"),
            )
            time_lbl.pack(side="right", padx=8, pady=12)
            self.lane_time_labels[lane] = time_lbl

        # Winner banner
        self.winner_frame = ctk.CTkFrame(self, fg_color="#2c3e50", corner_radius=12)
        self.winner_frame.pack(fill="x", padx=16, pady=(12, 16))

        self.lbl_winner = ctk.CTkLabel(
            self.winner_frame, text="",
            font=ctk.CTkFont(size=18, weight="bold"), text_color="#f1c40f",
        )
        self.lbl_winner.pack(pady=14)

        self._update_button_states()

    # ── BLE Thread ──

    def _start_ble_thread(self):
        self._ble_loop = asyncio.new_event_loop()
        self._ble_thread = threading.Thread(target=self._ble_loop.run_forever, daemon=True)
        self._ble_thread.start()

    def _run_coro(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self._ble_loop)

    # ── Connection ──

    def _on_connect(self):
        if self.connected:
            self._run_coro(self._disconnect())
        else:
            self.btn_connect.configure(text="Scanning...", state="disabled")
            self.lbl_status.configure(text="Scanning...", text_color="#f39c12")
            self._run_coro(self._scan_and_connect())

    async def _scan_and_connect(self):
        try:
            device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=8.0)
            if device is None:
                devices = await BleakScanner.discover(timeout=8.0)
                for d in devices:
                    if d.name and DEVICE_NAME.lower() in d.name.lower():
                        device = d
                        break

            if device is None:
                self.after(0, self._connection_failed, "Device not found")
                return

            self.after(0, lambda: self.lbl_status.configure(
                text="Connecting...", text_color="#f39c12"
            ))

            self.client = BleakClient(device, disconnected_callback=self._on_disconnect_cb)
            await self.client.connect()
            await self.client.start_notify(EVT_UUID, self._on_notification)
            self.connected = True
            self.after(0, self._connection_success)

        except Exception as e:
            self.after(0, self._connection_failed, str(e))

    def _connection_success(self):
        self.lbl_status.configure(text="Connected", text_color="#2ecc71")
        self.btn_connect.configure(text="Disconnect", state="normal")
        self._update_button_states()

    def _connection_failed(self, msg):
        self.lbl_status.configure(text=f"Failed: {msg}", text_color="#e74c3c")
        self.btn_connect.configure(text="Scan & Connect", state="normal")

    def _on_disconnect_cb(self, client):
        self.connected = False
        self.race_running = False
        self.after(0, self._on_disconnected_ui)

    def _on_disconnected_ui(self):
        self.lbl_status.configure(text="Disconnected", text_color="#e74c3c")
        self.btn_connect.configure(text="Scan & Connect", state="normal")
        self._update_button_states()

    async def _disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        self.connected = False
        self.race_running = False
        self.after(0, self._on_disconnected_ui)

    # ── Commands ──

    def _build_cmd(self, cmd_type, mode=0):
        return struct.pack("<BBxx", cmd_type, mode)

    async def _send_cmd(self, cmd_type, mode=0):
        if not self.client or not self.client.is_connected:
            return
        data = self._build_cmd(cmd_type, mode)
        await self.client.write_gatt_char(CMD_UUID, data, response=False)

    def _on_start(self):
        mode = RACE_MODE_4 if self.mode_var.get() == "4 Contestants" else RACE_MODE_1
        self._reset_lane_display()
        self._run_coro(self._start_sequence(mode))

    async def _start_sequence(self, mode):
        await self._send_cmd(CMD_SET_MODE, mode)
        await asyncio.sleep(0.1)
        await self._send_cmd(CMD_START_RACE)

    def _on_new_race(self):
        self._run_coro(self._send_cmd(CMD_NEW_RACE))
        self.race_running = False
        self._reset_lane_display()
        self._update_button_states()

    # ── Notification Handler ──

    def _on_notification(self, sender, data: bytearray):
        if len(data) < 11:
            return
        event_type, mode, button_index, elapsed_ms, race_id = struct.unpack(
            "<BBBII", data[:11]
        )
        self.after(0, self._handle_event, event_type, mode, button_index, elapsed_ms, race_id)

    def _handle_event(self, event_type, mode, button_index, elapsed_ms, race_id):
        if event_type == EVT_START_ACK:
            self.race_running = True
            self.race_start_local = time.monotonic()
            self.lbl_winner.configure(text="Race in progress...")
            self._update_button_states()

        elif event_type == EVT_STOP_EVENT:
            if 1 <= button_index <= 4:
                self.lane_times[button_index] = elapsed_ms
                self.finish_order.append(button_index)
                pos = len(self.finish_order)
                self.lane_time_labels[button_index].configure(text=format_time(elapsed_ms))
                pos_text = {1: "1st", 2: "2nd", 3: "3rd", 4: "4th"}.get(pos, f"{pos}th")
                color = "#f1c40f" if pos == 1 else "#bdc3c7"
                self.lane_pos_labels[button_index].configure(text=pos_text, text_color=color)
                if pos == 1:
                    self.lane_frames[button_index].configure(
                        border_color="#f1c40f", border_width=3
                    )

        elif event_type == EVT_RACE_COMPLETE:
            self.race_running = False
            self.winner = button_index if button_index > 0 else None
            self.winner_time = elapsed_ms
            self.lbl_elapsed.configure(text=format_time(elapsed_ms))
            if self.winner:
                self.lbl_winner.configure(
                    text=f"Winner: Lane {self.winner}  —  {format_time(elapsed_ms)}"
                )
            else:
                self.lbl_winner.configure(text=f"Race Complete  —  {format_time(elapsed_ms)}")
            self._update_button_states()

        elif event_type == EVT_PONG:
            self.lbl_winner.configure(text="Pong received!")

        elif event_type == EVT_ERROR:
            err_names = {1: "Unknown command", 2: "Invalid state", 3: "Mode not allowed"}
            msg = err_names.get(button_index, f"Error 0x{button_index:02X}")
            self.lbl_winner.configure(text=f"Error: {msg}")

    # ── Elapsed Timer ──

    def _tick_elapsed(self):
        if self.race_running:
            elapsed_s = time.monotonic() - self.race_start_local
            self.lbl_elapsed.configure(text=format_time(int(elapsed_s * 1000)))
        self.after(47, self._tick_elapsed)

    # ── UI Helpers ──

    def _reset_lane_display(self):
        self.lane_times = {1: None, 2: None, 3: None, 4: None}
        self.winner = None
        self.winner_time = None
        self.finish_order = []
        self.lbl_elapsed.configure(text="00:00.000")
        self.lbl_winner.configure(text="")
        for lane in range(1, 5):
            self.lane_time_labels[lane].configure(text="--:--.---")
            self.lane_pos_labels[lane].configure(text="")
            self.lane_frames[lane].configure(
                border_color=LANE_COLORS[lane], border_width=2
            )

    def _update_button_states(self):
        if self.connected:
            self.btn_start.configure(state="normal" if not self.race_running else "disabled")
            self.btn_new.configure(state="normal")
            self.mode_menu.configure(state="normal" if not self.race_running else "disabled")
        else:
            self.btn_start.configure(state="disabled")
            self.btn_new.configure(state="disabled")
            self.mode_menu.configure(state="disabled")

    # ── Cleanup ──

    def _on_close(self):
        if self.connected:
            self._run_coro(self._disconnect())
            time.sleep(0.3)
        if self._ble_loop:
            self._ble_loop.call_soon_threadsafe(self._ble_loop.stop)
        self.destroy()


if __name__ == "__main__":
    app = RaceTimerApp()
    app.mainloop()
