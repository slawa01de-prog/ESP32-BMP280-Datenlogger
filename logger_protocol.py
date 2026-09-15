"""V74 parser: accepts V73 live packets, durable identities for V74 records."""
import math

def checksum(text):
    h=2166136261
    for b in text.encode('utf-8'):
        h=((h^b)*16777619)&0xffffffff
    return h

def parse_packet(line):
    parts=line.strip().split(',')
    if len(parts) not in (15,22) or parts[0]!='NODEDATA':
        raise ValueError('Unvollstaendige NODEDATA-Zeile')
    if len(parts)==22 and checksum(','.join(parts[:21]))!=int(parts[21],16):
        raise ValueError('Pruefsumme falsch')
    if not parts[1] or len(parts[1])>10:
        raise ValueError('Ungueltige Node-ID')
    p=dict(zip(('id','name','fw'),parts[1:4]))
    p['log']=int(parts[4])
    for key,index in [('raw_t',5),('temp',6),('raw_p',7),('press',8),('toff',9),('poff',10)]:
        p[key]=float(parts[index])
    p.update(mode=int(parts[11]),power=int(parts[12]),wake=int(parts[13]),mac=parts[14],extended=len(parts)==22)
    p.update(sequence=p['log'],session=0,elapsed_ms=0,epoch_ms=0,flags=3,interval=60)
    if p['extended']:
        p.update(zip(('sequence','session','elapsed_ms','epoch_ms','flags','interval'),map(int,parts[15:21])))
        if min(p[k] for k in ('sequence','session','elapsed_ms','epoch_ms'))<0 or not 1<=p['interval']<=3600:
            raise ValueError('Ungueltige Zeit/Sequenz')
    p['valid']=bool(p['flags']&2) and all(math.isfinite(p[k]) for k in ('raw_t','temp','raw_p','press','toff','poff'))
    return p

def packet_key(p):
    if p['extended']:
        return f"{p['id']}:{p['session']}:{p['sequence']}"
    # Legacy has no session/time: best effort only, not suitable for reliable replay.
    return f"legacy:{p['id']}:{p['log']}:{p['temp']}:{p['press']}"
