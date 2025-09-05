# SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
# from pytest_embedded_idf.dut import IdfDut

import re
import pytest

@pytest.mark.esp32
@pytest.mark.generic
def test_timekeeper_sync_and_print(dut):
    """
    Build/flash the app, then wait for the final single-line UART time.
    Example the app prints every second:
      '01:23:45 PM 05-09-2025 IST'
    We give a generous timeout to allow Wi-Fi + NTP.
    """
    # If you kept INFO logs and want to assert sync too, uncomment:
    # dut.expect(re.compile(r'Time synchronized'), timeout=120)

    # Match the UART time line your app prints once per second
    time_line = re.compile(r'\b\d{2}:\d{2}:\d{2} (AM|PM) \d{2}-\d{2}-\d{4} IST\b')
    dut.expect(time_line, timeout=180)
