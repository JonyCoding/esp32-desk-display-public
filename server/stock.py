"""Fetch real-time stock index quotes (SSE Composite & NASDAQ) from Tencent Finance & Sina."""
from __future__ import annotations

import json
import math
import re
import threading
import time
from datetime import datetime
from urllib.request import Request, build_opener, ProxyHandler
from urllib.parse import urlencode
from typing import Any
from zoneinfo import ZoneInfo

TENCENT_QUOTE_URL = 'https://qt.gtimg.cn/q=usIXIC'
TENCENT_SH_MIN_URL = 'https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=sh000001'
SINA_US_MIN_URL = 'https://stock.finance.sina.com.cn/usstock/api/jsonp.php/var/US_MinlineService.getMinline?symbol=.IXIC'
SGE_DELAY_URL = 'https://www.sge.com.cn/sjzx/yshqbg'
# Daily bars: POST instid=Au99.99 -> {"time": [[date, open, close, low, high], ...]}.
# This is the same table as /sjzx/quotation_daily_new, but as JSON, and it keeps
# publishing the last trading day after the live quote page zeroes out.
SGE_DAILY_URL = 'https://www.sge.com.cn/graph/Dailyhq'
GOLD_SPOT_URL = 'https://api.gold-api.com/price/XAU/USD'
SHANGHAI = ZoneInfo('Asia/Shanghai')
NEW_YORK = ZoneInfo('America/New_York')
MARKET_REFRESH_SECONDS = 30 * 60
MARKET_OPEN_SECONDS = 5 * 60

# Two openers, tried in order by _read(). A home network has no proxy at all, a
# company laptop usually must go through one, and a wrapper process can inject
# HTTPS_PROXY that fails the TLS handshake — which is exactly how XAU/USD ended
# up printing "--" on the panel. Trying direct first costs one extra connect at
# worst and keeps the page fed on either network.
_DIRECT = build_opener(ProxyHandler({}))
_SYSTEM = build_opener()


def _read(req: Request, timeout: float) -> bytes:
    """Fetch one URL: direct first, then through the shell's proxy settings."""
    last: OSError | None = None
    for opener in (_DIRECT, _SYSTEM):
        try:
            with opener.open(req, timeout=timeout) as resp:
                return resp.read()
        except OSError as exc:
            last = exc
    raise last if last else OSError('no opener available')


def market_open(now: datetime) -> bool:
    """True while any tracked venue is trading, so the panel can refresh fast.

    SGE trades a day session plus a night session that crosses midnight
    (20:00-02:30), the SSE has the usual two blocks, and NASDAQ follows New
    York. Holidays are not in the calendar: a closed market simply stops
    answering with fresh bars, which the quote parsers already reject.
    """
    sh = now.astimezone(SHANGHAI)
    minute = sh.hour * 60 + sh.minute
    weekday = sh.weekday()  # Monday = 0
    sge_day = weekday < 5 and (9 * 60 <= minute <= 11 * 60 + 30 or
                               13 * 60 + 30 <= minute <= 15 * 60 + 30)
    sge_night = (weekday < 5 and minute >= 20 * 60) or (1 <= weekday <= 5 and minute <= 2 * 60 + 30)
    sse = weekday < 5 and (9 * 60 + 15 <= minute <= 11 * 60 + 35 or
                           12 * 60 + 55 <= minute <= 15 * 60 + 5)
    ny = now.astimezone(NEW_YORK)
    nasdaq = ny.weekday() < 5 and 9 * 60 + 30 <= ny.hour * 60 + ny.minute <= 16 * 60
    return sge_day or sge_night or sse or nasdaq

class StockMonitor:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.last_fetch = 0.0
        self.cached_data: dict[str, Any] = {
            'available': False,
            'sh': {'name': '上证指数', 'code': '000001', 'price': 0.0, 'change': 0.0, 'pct': 0.0, 'trend': []},
            'nasdaq': {'name': '纳斯达克', 'code': '.IXIC', 'price': 0.0, 'change': 0.0, 'pct': 0.0, 'trend': []},
            'updated': 0
        }
        self._thread: threading.Thread | None = None
        self.gold_price: float | None = None
        self.gold_usd: dict[str, Any] | None = None
        self.next_gold_fetch = 0.0
        # Last published Au99.99 close, used whenever the live page has zeroed
        # out (weekends, holidays, the 15:30-20:00 gap).
        self.gold_close: dict[str, Any] | None = None
        self.next_close_fetch = 0.0

    @staticmethod
    def _parse_gold_price(html: str) -> float | None:
        match = re.search(r'<td\b[^>]*>\s*Au99\.99\s*</td>\s*<td\b[^>]*>\s*'
                          r'(?:<span\b[^>]*>\s*)?(\d+(?:\.\d+)?)\s*(?:</span>\s*)?</td>', html, re.I)
        if match:
            price = float(match[1])
            if math.isfinite(price) and 0 < price < 10000:
                return price
        return None

    def _fetch_gold(self) -> float | None:
        try:
            req = Request(SGE_DELAY_URL, headers={'User-Agent': 'Mozilla/5.0'})
            return self._parse_gold_price(_read(req, 4.0).decode('utf-8', errors='replace'))
        except OSError:
            return None

    @staticmethod
    def _parse_gold_close(payload: str, now: float) -> dict[str, Any] | None:
        """Last daily bar for Au99.99: {'date': '2026-11-20', 'price': 764.55}.

        A close older than 30 days is not "the last trading day" any more, it is
        a broken feed, and printing it would be worse than printing nothing.
        """
        try:
            rows = json.loads(payload)['time']
            date, close = str(rows[-1][0]), float(rows[-1][2])
        except (KeyError, IndexError, TypeError, ValueError):
            return None
        if not (math.isfinite(close) and 0 < close < 10000):
            return None
        try:
            published = datetime.strptime(date, '%Y-%m-%d').replace(tzinfo=SHANGHAI)
        except ValueError:
            return None
        if not -86400 <= now - published.timestamp() <= 30 * 86400:
            return None
        return {'date': date, 'price': close}

    def _fetch_gold_close(self) -> dict[str, Any] | None:
        try:
            body = urlencode({'instid': 'Au99.99'}).encode()
            req = Request(SGE_DAILY_URL, data=body, headers={
                'User-Agent': 'Mozilla/5.0',
                'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8',
                'X-Requested-With': 'XMLHttpRequest',
                'Referer': 'https://www.sge.com.cn/sjzx/mrhq',
            })
            return self._parse_gold_close(_read(req, 4.0).decode('utf-8', errors='replace'), time.time())
        except OSError:
            return None

    @staticmethod
    def _parse_gold_usd(data: dict[str, Any], now: float) -> dict[str, Any] | None:
        try:
            if data['symbol'] != 'XAU' or data['currency'] != 'USD' or type(data['price']) not in (int, float):
                return None
            price = float(data['price'])
            updated = datetime.fromisoformat(data['updatedAt'].replace('Z', '+00:00'))
            if not (updated.tzinfo and -300 <= now - updated.timestamp() <= 4 * 86400 and 0 < price < 100000):
                return None
            return {'code': 'XAU/USD', 'price': price, 'unit': 'USD/oz', 'updated': int(updated.timestamp())}
        except (AttributeError, KeyError, TypeError, ValueError, OverflowError):
            return None

    def _fetch_gold_usd(self) -> dict[str, Any] | None:
        try:
            req = Request(GOLD_SPOT_URL, headers={'User-Agent': 'DeskDisplay/0.2'})
            return self._parse_gold_usd(json.loads(_read(req, 5.0)), time.time())
        except (OSError, ValueError):
            return None

    @staticmethod
    def _chart(prices: list[float], quote: dict[str, Any]) -> tuple[list[int], int]:
        price = quote['price']
        previous_close = price - quote['change']
        if (len(prices) < 2 or not all(math.isfinite(p) and p > 0
                                      for p in [price, previous_close, *prices])):
            return [], -1
        # Quotes and minute bars arrive independently. Do not present a
        # mismatched endpoint as the quoted price or invent a replacement.
        if abs(prices[-1] - price) > max(0.05, price * 0.0005):
            return [], -1
        count = min(20, len(prices))
        sampled = [prices[round(i * (len(prices) - 1) / (count - 1))]
                   for i in range(count)]
        low, high = min(*sampled, previous_close), max(*sampled, previous_close)
        if high - low < 0.001:
            return [9] * count, 9

        def position(p: float) -> int:
            return round((p - low) / (high - low) * 18)

        return [position(p) for p in sampled], position(previous_close)

    @classmethod
    def _add_trend(cls, quote: dict[str, Any], points: list[tuple[int, float]], session_span: int) -> None:
        quote['trend'], quote['trendRef'] = cls._chart([price for _, price in points], quote)
        quote['trendX'] = []
        count = len(quote['trend'])
        if count:
            quote['sessionMinute'] = points[-1][0]
            quote['trendX'] = [round(points[round(i * (len(points) - 1) / (count - 1))][0] * 57 / session_span)
                               for i in range(count)]

    @classmethod
    def _parse_sh_session(cls, panel: dict[str, Any]) -> dict[str, Any]:
        session = panel['data']
        qt = panel['qt']['sh000001']
        date = session['date']
        if qt[30][:8] != date:
            raise ValueError('SSE quote and minute bars are from different sessions')
        price, close = float(qt[3]), float(qt[4])
        if not all(math.isfinite(p) and p > 0 for p in (price, close)):
            raise ValueError('invalid SSE price')
        quote = {'name': '上证指数', 'code': '000001', 'price': price,
                 'change': round(price - close, 2),
                 'pct': round((price - close) / close * 100, 2),
                 'session': date, 'trend': [], 'trendRef': -1, 'trendX': []}
        by_minute: dict[int, float] = {}
        for row in session['data']:
            clock, value, *_ = row.split()
            hour, minute = int(clock[:2]), int(clock[2:])
            if not (0 <= minute < 60):
                continue
            at = hour * 60 + minute
            if 570 <= at <= 690:
                by_minute[at - 570] = float(value)
            elif 780 <= at <= 900:
                by_minute[at - 660] = float(value)
        points = sorted(by_minute.items())
        cls._add_trend(quote, points, 240)
        return quote

    def _fetch_sh_session(self) -> dict[str, Any] | None:
        try:
            req = Request(TENCENT_SH_MIN_URL, headers={'User-Agent': 'Mozilla/5.0'})
            return self._parse_sh_session(json.loads(_read(req, 3.0))['data']['sh000001'])
        except (OSError, KeyError, IndexError, TypeError, ValueError):
            return None

    @classmethod
    def _parse_us_session(cls, fields: list[str], minutes: dict[str, Any], now: datetime) -> dict[str, Any]:
        timestamp = datetime.strptime(fields[30], '%Y-%m-%d %H:%M:%S')
        market_now = now.astimezone(NEW_YORK)
        if fields[2] != '.IXIC' or timestamp.weekday() >= 5 or timestamp.date() > market_now.date():
            raise ValueError('invalid NASDAQ session')
        price, close = float(fields[3]), float(fields[4])
        if not all(math.isfinite(p) and p > 0 for p in (price, close)):
            raise ValueError('invalid NASDAQ quote')

        item = minutes['minline_1'][0]
        first = item['first_min']
        started = datetime.strptime(f'{first[0]} {first[1]}', '%Y-%m-%d %H:%M:%S')
        if started.date() != timestamp.date() or abs(float(first[2]) - close) > 0.05:
            raise ValueError('NASDAQ quote and minute bars are from different sessions')
        opening, regular_span = 9 * 60 + 30, 390
        offset = started.hour * 60 + started.minute - opening
        if offset < 0 or offset > regular_span:
            raise ValueError('NASDAQ minutes start outside regular trading')
        prices = [first[3], *(row[0] if row else None for row in item['other_min'])]
        quote = {'name': '纳斯达克', 'code': '.IXIC', 'price': price,
                 'change': round(price-close, 2), 'pct': round((price-close)/close*100, 2),
                 'session': started.strftime('%Y%m%d')}
        points: list[tuple[int, float]] = []
        last_minute = min(regular_span, timestamp.hour * 60 + timestamp.minute - opening)
        if started.date() == market_now.date():
            last_minute = min(last_minute, market_now.hour * 60 + market_now.minute - opening)
        for minute, raw in enumerate(prices, offset):
            if minute > last_minute:
                break
            try:
                value = float(raw)
            except (TypeError, ValueError):
                continue
            if math.isfinite(value) and value > 0:
                points.append((minute, value))

        # A shortened session stops at its final actual minute on the regular axis.
        cls._add_trend(quote, points, regular_span)
        return quote

    def _fetch_us_session(self) -> dict[str, Any] | None:
        try:
            headers = {'User-Agent': 'Mozilla/5.0'}
            raw = _read(Request(TENCENT_QUOTE_URL, headers=headers), 3.0).decode('gbk')
            fields = raw.split('"')[1].split('~')
            raw = _read(Request(SINA_US_MIN_URL, headers=headers), 3.0).decode('utf-8')
            match = re.search(r'var\((\{.*\})\)', raw, re.S)
            if match:
                return self._parse_us_session(fields, json.loads(match[1]), datetime.now(NEW_YORK))
        except (OSError, KeyError, IndexError, TypeError, ValueError):
            return None
        return None

    def fetch_quotes(self) -> dict[str, Any] | None:
        result: dict[str, Any] = {'updated': int(time.time())}
        sh = self._fetch_sh_session()
        if sh:
            # "closed" means the numbers belong to an earlier session, which is
            # what the panel dims. Comparing dates is exact; guessing from the
            # clock is not (holidays, shortened sessions).
            sh['closed'] = sh['session'] != datetime.now(SHANGHAI).strftime('%Y%m%d')
            result['sh'] = sh
        us = self._fetch_us_session()
        if us:
            us['closed'] = us['session'] != datetime.now(NEW_YORK).strftime('%Y%m%d')
            result['nasdaq'] = us
        now = time.monotonic()
        if now >= self.next_gold_fetch:
            new_gold = self._fetch_gold()
            if new_gold is not None:
                self.gold_price = new_gold
            new_usd = self._fetch_gold_usd()
            if new_usd is not None:
                self.gold_usd = new_usd

            if self.gold_price is None and now >= self.next_close_fetch:
                close = self._fetch_gold_close()
                if close is not None:
                    self.gold_close = close
                # A close changes once a day: retry soon if the feed failed,
                # otherwise leave it alone for six hours.
                self.next_close_fetch = now + (6 * 3600 if self.gold_close else 300)

            # Fast retry (30s) if we have no gold data at all; 300s normally
            has_gold = (self.gold_price is not None) or (self.gold_close is not None)
            self.next_gold_fetch = now + (300 if has_gold and self.gold_usd else 30)
        if self.gold_price is not None:
            result['gold'] = {'code': 'Au99.99', 'price': self.gold_price,
                              'unit': 'CNY/g', 'delayed': True, 'closed': False}
        elif self.gold_close is not None:
            # The live page zeroes out when the market is shut; the daily bar is
            # the last thing that actually traded.
            result['gold'] = {'code': 'Au99.99', 'price': self.gold_close['price'],
                              'unit': 'CNY/g', 'delayed': True, 'closed': True,
                              'session': self.gold_close['date']}
        if self.gold_usd is not None:
            # Spot gold stops ticking at the weekend too. Fifteen minutes of
            # silence is already a shut market for a quote that updates by the
            # second, and the panel dims the line instead of implying it is live.
            self.gold_usd['closed'] = time.time() - self.gold_usd['updated'] > 900
            result['gold_usd'] = self.gold_usd
        result['available'] = bool(result.keys() & {'sh', 'nasdaq', 'gold', 'gold_usd'})
        return result

    def poll_once(self) -> None:
        data = self.fetch_quotes()
        if data:
            with self.lock:
                for market in ('sh', 'nasdaq'):
                    previous = self.cached_data.get(market, {})
                    current = data.get(market, {})
                    if previous.get('trend') and (not current.get('trend') or
                            (previous['session'], previous['sessionMinute']) >
                            (current['session'], current['sessionMinute'])):
                        data[market] = previous
                for item in ('gold', 'gold_usd'):
                    if item not in data and item in self.cached_data:
                        data[item] = self.cached_data[item]
                data['available'] = bool(data.keys() & {'sh', 'nasdaq', 'gold', 'gold_usd'})
                self.cached_data = data
                self.last_fetch = time.time()

    def get_stock(self) -> dict[str, Any]:
        with self.lock:
            return dict(self.cached_data)

    def start(self) -> None:
        def loop():
            while True:
                try:
                    self.poll_once()
                except Exception:
                    pass
                # 5 minutes while something is trading, 30 when nothing is:
                # a desk index that lags half an hour is not an index.
                open_now = market_open(datetime.now(SHANGHAI))
                time.sleep(MARKET_OPEN_SECONDS if open_now else MARKET_REFRESH_SECONDS)
        self._thread = threading.Thread(target=loop, name='stock-monitor', daemon=True)
        self._thread.start()

STOCK_MONITOR = StockMonitor()
