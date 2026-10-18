from __future__ import annotations

import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).parents[1] / "tools" / "wireless_client.py"
SPEC = importlib.util.spec_from_file_location("wireless_client", MODULE_PATH)
assert SPEC and SPEC.loader
wireless_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wireless_client)


class FakeClient:
    def __init__(self) -> None:
        self.mode = "normal"
        self.calls = []

    def state(self):
        return {"mode": self.mode, "gridOnline": self.mode != "outage", "homes": []}

    def set_stage(self, stage):
        self.mode = stage
        self.calls.append(("stage", stage))

    def set_grid(self, online):
        self.mode = "grid" if online else "outage"
        self.calls.append(("grid", online))

    def set_load(self, home, load, field, enabled):
        self.calls.append(("load", home, load, field, enabled))


class CommandTests(unittest.TestCase):
    def test_numeric_stage(self):
        client = FakeClient()
        message, finished = wireless_client.execute_command(client, ["2"])
        self.assertFalse(finished)
        self.assertEqual(client.calls, [("stage", "grid")])
        self.assertIn("grid-connected", message)

    def test_enter_advances_stage(self):
        client = FakeClient()
        wireless_client.execute_command(client, [])
        self.assertEqual(client.calls, [("stage", "grid")])

    def test_grid_outage(self):
        client = FakeClient()
        wireless_client.execute_command(client, ["grid", "down"])
        self.assertEqual(client.calls, [("grid", False)])

    def test_load_numbers_convert_to_api_index(self):
        client = FakeClient()
        wireless_client.execute_command(client, ["load", "1", "2", "allow", "on"])
        self.assertEqual(client.calls, [("load", 1, 2, "allow", True)])

    def test_bad_command(self):
        with self.assertRaises(wireless_client.ClientError):
            wireless_client.execute_command(FakeClient(), ["explode"])


if __name__ == "__main__":
    unittest.main()
