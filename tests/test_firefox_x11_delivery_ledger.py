#!/usr/bin/env python3
import unittest
from firefox_x11_delivery_ledger import (observe_png_delivery,
    wait_for_png_delivery, DeliveryTraceError)

GEN = 12
N = 10

def log_event(event, **fields):
    return 'XRDP_CONSOLE_CLIPBOARD_IMAGE event=' + event + ' ' + ' '.join(
        f'{k}={v}' for k, v in fields.items())

def request(xid, prop, mono, gen=GEN, owner='0xdead'):
    return log_event('x11-request', target='image/png', requestor=xid,
                     owner=owner, property=prop, generation=gen, mono_ns=mono)

def notify(xid, prop, mono, owner='0xdead'):
    return log_event('x11-selection-notify-issued', path='incr',
                     target='image/png', requestor=xid, owner=owner,
                     property=prop, mono_ns=mono)

def chunk(xid, prop, mono, byte_count=N, gen=GEN):
    return log_event('x11-incr-chunk-issued', target='image/png',
                     requestor=xid, property=prop, mono_ns=mono, bytes=byte_count,
                     chunk=1, start_generation=gen, current_generation=gen,
                     state_match=1)

def ack(xid, prop, mono, gen=GEN, match=1):
    return log_event('x11-incr-terminator-ack', target='image/png',
                     requestor=xid, property=prop, mono_ns=mono,
                     terminator_generation=gen, current_generation=gen,
                     start_generation=gen, state_match=match)

def three_retries():
    items=[]
    for i, xid in enumerate(('0x101','0x102','0x103')):
        items.append(request(xid, f'0x{i+201:x}', i*1000))
    for i,xid in enumerate(('0x101','0x102','0x103')):
        prop=f'0x{i+201:x}'
        items.extend([notify(xid,prop,3000+i*100),
                      chunk(xid,prop,3020+i*100), ack(xid,prop,3040+i*100)])
    return items

class Tests(unittest.TestCase):
    def status(self, lines):
        return observe_png_delivery('\n'.join(lines),generation=GEN,expected_bytes=N)

    def assert_inconclusive(self, lines):
        with self.assertRaises(DeliveryTraceError):self.status(lines)

    def test_high_atom_id_unresolved_terminal_ack_is_still_attributed(self):
        # PR #32's get_atom_text() cannot resolve certain atom IDs >512.
        # The emitted source log can say 'target=unknown atom 0x...' while
        # retaining the exact X11 requestor/property/generation identity.
        lines = three_retries()
        lines[-1] = lines[-1].replace(
            "target=image/png", "target=unknown atom 0x241")
        report = self.status(lines)
        self.assertTrue(report["complete"])
        self.assertEqual(report["ack_count"], 3)
        self.assertEqual(report["verified_png_bytes"], N * 3)

    def test_unresolved_terminal_ack_cannot_be_borrowed_from_other_requestor(self):
        lines = three_retries()
        lines[-1] = ack("0x777", "0xcb", 3240).replace(
            "target=image/png", "target=unknown atom 0x241")
        with self.assertRaises(DeliveryTraceError):
            self.status(lines)

    def test_known_bmp_terminal_ack_cannot_complete_png_transfer(self):
        lines = three_retries()
        lines[-1] = lines[-1].replace("target=image/png", "target=image/bmp")
        self.assertFalse(self.status(lines)["complete"])

    def test_three_retries_completed(self):
        summary=self.status(three_retries())
        self.assertTrue(summary['complete'])
        self.assertEqual(summary['request_count'],3)
        self.assertEqual(summary['ack_count'],3)
        self.assertEqual(summary['verified_png_bytes'],N*3)
    def test_pending_incomplete_response(self):
        self.assertFalse(self.status(three_retries()[:-1])['complete'])
    def test_pending_no_selection(self):
        self.assertEqual(self.status([])['reason'],'no-image-requests')
    def test_reused_requestor_wrong_property(self):
        broken=three_retries()
        broken[-1]=ack('0x103','0xfefe',3240)
        self.assert_inconclusive(broken)
    def test_wrong_generation_ack(self):
        broken=three_retries()
        broken[-1]=ack('0x103','0xcb',3240,gen=GEN+1)
        self.assert_inconclusive(broken)
    def test_wrong_generation_request(self):
        broken=three_retries()
        broken[0]=request('0x101','0xc9',0,gen=GEN+1)
        self.assert_inconclusive(broken)
    def test_wrong_owner_notify(self):
        broken=three_retries()
        broken[3]=notify('0x101','0xc9',3000,owner='0xdef')
        self.assert_inconclusive(broken)
    def test_missing_chunk(self):
        broken=three_retries()
        del broken[4]
        self.assert_inconclusive(broken)
    def test_truncated_png_bytes(self):
        broken=three_retries()
        broken[4]=chunk('0x101','0xc9',3020,byte_count=N-1)
        self.assert_inconclusive(broken)
    def test_wrong_delivery_state(self):
        broken=three_retries()
        broken[-1]=ack('0x103','0xcb',3240,match=0)
        self.assert_inconclusive(broken)
    def test_stale_generation_chunk(self):
        broken=three_retries()
        broken[4]=chunk('0x101','0xc9',3020,gen=GEN-1)
        self.assert_inconclusive(broken)
    def test_duplicate_notify(self):
        broken=three_retries()
        broken.insert(4,notify('0x101','0xc9',3001))
        self.assert_inconclusive(broken)
    def test_ack_before_notify(self):
        broken=three_retries()
        broken[-1]=ack('0x103','0xcb',3199)
        self.assert_inconclusive(broken)
    def test_reused_identical_xid_property_counts(self):
        lines=[request('0x101','0xc9',0), request('0x101','0xc9',1000),
               notify('0x101','0xc9',3000), chunk('0x101','0xc9',3010),
               ack('0x101','0xc9',3020),
               notify('0x101','0xc9',3100),chunk('0x101','0xc9',3110),
               ack('0x101','0xc9',3120)]
        self.assertEqual(self.status(lines)['ack_count'],2)
        self.assertTrue(self.status(lines)['complete'])
    def test_waits_for_late_second_ack(self):
        timeline=[
            '\n'.join(three_retries()[:-1]),
            '\n'.join(three_retries()),
        ]
        class Clock:
            current=0.0
            def now(self): return self.current
            def pause(self,dt): self.current += dt
        clock=Clock()
        def read():return timeline[0] if clock.current<0.3 else timeline[1]
        result=wait_for_png_delivery(read,lambda:True,generation=GEN,
            expected_bytes=N,timeout_s=2,quiescence_s=.35,
            clock=clock.now,pause=clock.pause)
        self.assertEqual(result['ack_count'],3)
        self.assertGreaterEqual(clock.current,0.65)
    def test_wait_timeout_for_incomplete(self):
        class Clock:
            current=0.0
            def now(self):return self.current
            def pause(self,dt):self.current+=dt
        clock=Clock()
        with self.assertRaises(DeliveryTraceError):
            wait_for_png_delivery(lambda:'\n'.join(three_retries()[:-1]),
                lambda:True, generation=GEN,expected_bytes=N,
                timeout_s=.3,clock=clock.now,pause=clock.pause)
    def test_does_not_claim_browser_success(self):
        result=self.status(three_retries())
        self.assertNotIn('browser_classification',result)
        self.assertNotIn('file_sha256',result)

if __name__ == '__main__': unittest.main()
