"""Pieces of the bridge that do not depend on the operating system.

Both hosts import this: the Linux daemon (MPRIS + PulseAudio) and the Windows
one (media keys + SMTC + Core Audio). Keeping the wire protocol, the text
sanitising, the price formatting and the CoinGecko client in one place means
the device sees exactly the same frames whichever machine it is plugged into.
"""

from __future__ import annotations

import json
import logging
import threading
import time
import unicodedata
import urllib.error
import urllib.request

LOG = logging.getLogger("media-remote")

POLL_INTERVAL = 0.5  # seconds between status frames sent to the device
CRYPTO_INTERVAL = 300  # seconds between price refreshes
CRYPTO_FRAME_INTERVAL = 5  # seconds between price frames sent to the device
CRYPTO_URL = (
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids={ids}&vs_currencies={cur}&include_24hr_change=true"
)
CRYPTO_CHART_URL = (
    "https://api.coingecko.com/api/v3/coins/{coin}/market_chart"
    "?vs_currency={cur}&days=1"
)
# The device picks from this set; anything else is refused rather than pasted
# into a URL. Keep it in step with CATALOG in the firmware.
ALLOWED_COINS = frozenset({
    "bitcoin", "ethereum", "cardano", "solana", "ripple", "dogecoin",
    "polkadot", "chainlink", "avalanche-2", "matic-network", "litecoin",
    "bitcoin-cash", "tron", "cosmos", "uniswap", "stellar", "algorand",
    "near", "aptos", "arbitrum", "optimism", "shiba-inu", "the-open-network",
    "sui",
})
MAX_COINS = 3

# The currencies the settings panel offers, and how a reader of each expects
# the digits grouped. Not cosmetic: European convention puts a space between
# thousands and a comma before the decimals, while the dollar and the pound do
# the reverse, so "802,234" and "802 234" are the same number to different
# people. The device sends a code from this set and nothing else is accepted.
# Keep it in step with CURRENCIES in the firmware.
CURRENCIES = {
    "NOK": (" ", ","),
    "USD": (",", "."),
    "EUR": (" ", ","),
    "GBP": (",", "."),
    "SEK": (" ", ","),
}
DEFAULT_CURRENCY = "NOK"
CHART_SPACING = 20  # seconds between chart calls; CoinGecko's free tier is
                    # strict enough that three back-to-back requests earn a 429
SPARK_POINTS = 32  # samples per 24 h sparkline
SPARK_MAX = 63  # each sample is scaled to 0..SPARK_MAX within its own range
MAX_TITLE = 48
MAX_ARTIST = 48

# How far next/previous move the position when the player has no such track --
# a browser tab playing a single video is the case that matters. Both hosts
# read it from here so the remote behaves the same on either machine.
SEEK_SECONDS = 10

# The display uses the Arduino GFX built-in font, which is ASCII only.
TRANSLIT = {
    "\u00e6": "ae", "\u00c6": "AE", "\u00f8": "o", "\u00d8": "O",
    "\u00e5": "a", "\u00c5": "A", "\u00df": "ss",
    "\u2013": "-", "\u2014": "-", "\u2019": "'", "\u2018": "'",
    "\u201c": '"', "\u201d": '"', "\u2026": "...", "\u00b7": "-",
}


def format_price(value: float, currency: str = DEFAULT_CURRENCY) -> str:
    """Render a price for a 170 px wide screen, in the reader's convention.

    The catalogue spans Bitcoin at ~800 000 kr and Shiba Inu at ~0.0001 kr, so
    no single fixed-point integer carries both ends: micro-units overflow an
    int32 at the top, and hundredths round to zero at the bottom. The host has
    the float, so the host places the decimal point.
    """
    thousands, decimal = CURRENCIES.get(currency, CURRENCIES[DEFAULT_CURRENCY])
    if value >= 1000:
        return f"{value:,.0f}".replace(",", thousands)
    if value >= 1:
        return f"{value:.2f}".replace(".", decimal)
    # Sub-unit coins: keep four significant digits rather than a row of zeros.
    text = f"{value:.8f}".rstrip("0")
    digits = text.split(".")[1]
    lead = len(digits) - len(digits.lstrip("0"))
    return f"{value:.{min(lead + 4, 8)}f}".rstrip("0").replace(".", decimal)


def to_ascii(text: str) -> str:
    for src, dst in TRANSLIT.items():
        text = text.replace(src, dst)
    text = unicodedata.normalize("NFKD", text)
    text = text.encode("ascii", "ignore").decode("ascii")
    # '|' is the wire protocol's field separator.
    return text.replace("|", "/").strip()



# ----------------------------------------------------------------- crypto


class Prices:
    """Polls CoinGecko for spot prices on a background thread.

    It runs off the main loop because a blocking fetch there would stall the
    heartbeat the device uses to decide the host is still alive.
    """

    DEFAULT_COINS = ("cardano", "bitcoin", "ethereum")

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._coins = list(self.DEFAULT_COINS)
        self._currency = DEFAULT_CURRENCY
        # coin -> (display string, change in tenths of a percent)
        self._data: dict[str, tuple[str, int]] = {}
        self._spark: dict[str, list[int]] = {}  # coin -> SPARK_POINTS samples
        self._ok = False
        self._wake = threading.Event()
        thread = threading.Thread(target=self._run, name="prices", daemon=True)
        thread.start()

    @property
    def coins(self) -> list[str]:
        with self._lock:
            return list(self._coins)

    @property
    def currency(self) -> str:
        with self._lock:
            return self._currency

    def set_currency(self, code: str) -> bool:
        """Adopt a currency from the device. Returns True if it changed.

        Everything cached is priced in the old currency, so it all goes --
        showing dollar figures under a NOK heading for five minutes would be
        worse than showing "--" for a second.
        """
        code = code.strip().upper()
        if code not in CURRENCIES:
            LOG.warning("ignoring unknown currency %r", code)
            return False
        with self._lock:
            if code == self._currency:
                return False
            self._currency = code
            self._data.clear()
            self._spark.clear()
            self._ok = False
        LOG.info("currency -> %s", code)
        self._wake.set()  # refetch now instead of at the next interval
        return True

    def set_coins(self, ids: list[str]) -> bool:
        """Adopt a new selection from the device. Returns True if it changed."""
        clean = [c for c in ids if c in ALLOWED_COINS][:MAX_COINS]
        if not clean:
            LOG.warning("ignoring empty/unknown coin selection %r", ids)
            return False
        with self._lock:
            if clean == self._coins:
                return False
            self._coins = clean
            # The old prices belong to coins that are no longer on screen.
            self._data.clear()
            self._spark.clear()
            self._ok = False
        LOG.info("coins -> %s", ", ".join(clean))
        self._wake.set()  # refetch now instead of at the next interval
        return True

    def _run(self) -> None:
        while True:
            self._wake.clear()
            wanted = self.coins
            try:
                self._fetch(wanted)
            except Exception as exc:  # network, JSON, anything: keep last values
                LOG.warning("price fetch failed: %s", exc)

            for coin in wanted:
                # Interruptible on purpose: a coin or currency change should not
                # have to wait out a chart-spacing sleep before the screen
                # catches up. wait() returns True the moment it is woken, and
                # False once the full interval has passed.
                if self._wake.wait(timeout=CHART_SPACING):
                    break  # the selection changed; start over on the new list
                try:
                    self._fetch_chart(coin)
                except Exception as exc:
                    LOG.warning("%s chart fetch failed: %s", coin, exc)

            spent = CHART_SPACING * len(wanted)
            self._wake.wait(timeout=max(60, CRYPTO_INTERVAL - spent))

    @staticmethod
    def _get_json(url: str):
        req = urllib.request.Request(
            url, headers={"User-Agent": "esp32-media-remote/1.0"}
        )
        for attempt in range(3):
            try:
                with urllib.request.urlopen(req, timeout=15) as resp:
                    return json.load(resp)
            except urllib.error.HTTPError as exc:
                if exc.code != 429 or attempt == 2:
                    raise
                # Back off and try again; the free tier throttles in bursts.
                time.sleep(15 * (attempt + 1))
        raise RuntimeError("unreachable")

    def _fetch_chart(self, coin: str) -> None:
        cur = self.currency
        raw = self._get_json(
            CRYPTO_CHART_URL.format(coin=coin, cur=cur.lower()))
        series = [p[1] for p in raw.get("prices", []) if p and p[1] is not None]
        if len(series) < 4:
            raise ValueError("chart too short")

        # Even downsample, then scale into the coin's own 24 h range -- the
        # sparkline shows shape, and the figure beside it carries the level.
        step = (len(series) - 1) / (SPARK_POINTS - 1)
        picked = [series[round(i * step)] for i in range(SPARK_POINTS)]
        lo, hi = min(picked), max(picked)
        span = hi - lo
        if span <= 0:
            scaled = [SPARK_MAX // 2] * SPARK_POINTS
        else:
            scaled = [round((v - lo) / span * SPARK_MAX) for v in picked]

        with self._lock:
            if coin not in self._coins:
                return  # dropped from the selection mid-fetch
            self._spark[coin] = scaled

    def _fetch(self, wanted: list[str]) -> None:
        cur = self.currency
        key = cur.lower()
        raw = self._get_json(
            CRYPTO_URL.format(ids=",".join(wanted), cur=key))

        fresh = {}
        for coin in wanted:
            entry = raw.get(coin) or {}
            price = entry.get(key)
            change = entry.get(f"{key}_24h_change")
            if price is None:
                continue
            fresh[coin] = (format_price(price, cur),
                           round((change or 0.0) * 10))

        if not fresh:
            raise ValueError("no usable prices in response")
        with self._lock:
            if self._coins != wanted or self._currency != cur:
                return  # the selection moved on while we were fetching
            self._data = fresh
            self._ok = True
        LOG.info("prices (%s): %s", cur,
                 {c: v[0] for c, v in fresh.items()})

    def frame(self) -> str:
        """CX|ok|count|price|chg|... -- one pair per selected coin.

        Prices are pre-formatted display strings; changes are tenths of a
        percent over 24 h.
        """
        with self._lock:
            ok = self._ok
            data = dict(self._data)
            wanted = list(self._coins)
        parts = ["CX", "1" if ok else "0", str(len(wanted))]
        for coin in wanted:
            price, change = data.get(coin, ("--", 0))
            parts += [price, str(change)]
        return "|".join(parts) + "\n"

    def spark_frame(self, index: int) -> str | None:
        """CS|index|v,v,v,... -- one coin's 24 h shape, or None if not fetched."""
        with self._lock:
            if index >= len(self._coins):
                return None
            samples = self._spark.get(self._coins[index])
        if not samples:
            return None
        return f"CS|{index}|" + ",".join(str(v) for v in samples) + "\n"


