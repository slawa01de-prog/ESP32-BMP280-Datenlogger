import sys, unittest, tempfile, sqlite3, datetime as dt, queue, threading, time
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from logger_protocol import parse_packet,packet_key,checksum
import bmp280_logger_v74_gui as gui

def packet(seq=1,session=100,temp='23.5',epoch=1780000000000):
    body=f'NODEDATA,B123456789,Logger,V7.4,1,{temp},{temp},1000,1000,0,0,2,0,0,AA:BB:CC:DD:EE:FF,{seq},{session},1000,{epoch},7,60'
    return body+','+format(checksum(body),'x')

class ProtocolTests(unittest.TestCase):
    def test_valid_and_duplicate_identity(self):
        p=parse_packet(packet()); self.assertTrue(p['valid']); self.assertEqual(packet_key(p),packet_key(parse_packet(packet())))
    def test_new_boot_identity(self):
        self.assertNotEqual(packet_key(parse_packet(packet())),packet_key(parse_packet(packet(session=101))))
    def test_corrupt_value_detected(self):
        with self.assertRaises(ValueError): parse_packet(packet().replace('23.5','24.5',1))
    def test_truncation_detected(self):
        with self.assertRaises(ValueError): parse_packet(packet()[:-4])
    def test_nan_invalid_not_zero(self):
        self.assertFalse(parse_packet(packet(temp='nan'))['valid'])
    def test_v73_live_accepted(self):
        self.assertFalse(parse_packet(','.join(packet().split(',')[:15]))['extended'])

class WorkerTests(unittest.TestCase):
    def test_read_failure_clears_connection(self):
        class Port:
            is_open=True
            def read(self,n): raise OSError('USB removed')
            def close(self): self.is_open=False
        w=gui.SerialWorker(queue.Queue()); p=Port(); w.ser=p; w.running=True
        w.reader(p,threading.Event(),0)
        self.assertFalse(w.is_connected()); self.assertIsNone(w.ser); self.assertFalse(w.running)
        self.assertEqual(w.q.get()[0],'error'); self.assertEqual(w.q.get()[0],'disconnected')
    def test_old_reader_cannot_close_new_port(self):
        class Port:
            is_open=True
            def close(self): self.is_open=False
        w=gui.SerialWorker(queue.Queue()); old=Port(); new=Port(); w.ser=new; w.running=True
        stop=threading.Event(); stop.set(); w.reader(old,stop,0)
        self.assertIs(w.ser,new); self.assertTrue(new.is_open)
    def test_fragmented_lines(self):
        stop=threading.Event()
        class Port:
            is_open=True
            def __init__(self): self.fragments=iter([b'NODE',b'DATA,a\nACK,b\n'])
            def read(self,n):
                try:return next(self.fragments)
                except StopIteration:stop.set();return b''
            def close(self):self.is_open=False
        w=gui.SerialWorker(queue.Queue()); p=Port(); w.ser=p; w.running=True; w.reader(p,stop,0)
        events=[]
        while not w.q.empty():events.append(w.q.get())
        self.assertEqual([e[1] for e in events if e[0]=='rx'],['NODEDATA,a','ACK,b'])

class PipelineTests(unittest.TestCase):
    def test_bad_line_does_not_stop_poll(self):
        q=queue.Queue();q.put(('rx','broken',3));q.put(('rx','good',3));q.put(('rx','stale',2))
        received=[]; scheduled=[]
        def receive(line):
            if line=='broken':raise ValueError('bad')
            received.append(line)
        f=SimpleNamespace(q=q,serial=SimpleNamespace(generation=3),handle_line=receive,write_term=lambda s:None,
             invalid_packets=0,ui_dirty=False,sync_connection_indicator=lambda:None,archive=SimpleNamespace(commit=lambda:None),
             after=lambda *a:scheduled.append(a),poll=lambda:None)
        gui.App.poll(f)
        self.assertEqual(received,['good']);self.assertEqual(f.invalid_packets,1);self.assertEqual(len(scheduled),1)
    def test_archive_deduplicates_and_keeps_session(self):
        db=sqlite3.connect(':memory:');db.execute('CREATE TABLE samples (key TEXT PRIMARY KEY,node TEXT,stamp TEXT,line TEXT)')
        f=SimpleNamespace(seen_packets=set(),session_origins={},nodes={},loading_history=False,archive=db,
             status_msg=SimpleNamespace(config=lambda **k:None),update_top_status_from_node=lambda n:None,pc_csv_writer=None)
        gui.App.handle_nodedata(f,packet());gui.App.handle_nodedata(f,packet());gui.App.handle_nodedata(f,packet(session=101))
        self.assertEqual(len(f.nodes['B123456789']['rows']),2)
        self.assertEqual(db.execute('SELECT count(*) FROM samples').fetchone()[0],2)
        db.close()
    def test_redraw_has_no_recursive_timer(self):
        import ast, inspect, textwrap
        tree=ast.parse(textwrap.dedent(inspect.getsource(gui.App.redraw_graph)))
        self.assertFalse(any(isinstance(n,ast.Attribute) and n.attr=='after' for n in ast.walk(tree)))

if __name__=='__main__': unittest.main(verbosity=2)
