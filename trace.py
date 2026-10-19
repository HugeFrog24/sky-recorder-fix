import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


def analyze_observation(rows, dropped=0):
    rows = sorted(rows, key=lambda r: r['time_ns'])
    events = Counter(r['event'] for r in rows)
    registrations = {r['a']: r for r in rows if r['event'] == 'register'}
    callbacks = {r['a']: r for r in rows if r['event'] == 'callback_begin'}
    pixels = {r['a']: r for r in rows if r['event'] == 'pixels'}
    fence_samples = defaultdict(dict)
    for r in rows:
        if r['event'] == 'fence':
            fence_samples[r['a']][(r['b'], r['d'], r['e'])] = r['f']
    captures = defaultdict(list)
    for r in rows:
        if r['event'] == 'capture' and r['k']:
            captures[(r['a'], r['b'], r['e'])].append(r)
    submissions = []
    for end in rows:
        if end['event'] != 'submit_end' or end['f'] != 0:
            continue
        starts = [r for r in rows if r['event'] == 'submit_begin' and
                  (r['a'], r['b'], r['d'], r['e']) == (end['a'], end['b'], end['d'], end['e']) and
                  r['time_ns'] <= end['time_ns']]
        for start in starts:
            for capture in captures.get((start['a'], start['b'], start['f']), []):
                if capture['time_ns'] <= start['time_ns']:
                    submissions.append({'renderer': start['a'], 'serial': start['b'], 'fence': start['e'],
                                        'image': capture['h'], 'tex': capture['g'], 'time': end['time_ns']})
    observations = []
    for read in (r for r in rows if r['event'] == 'read'):
        ident = read['a']
        reg = registrations.get(ident)
        pix = pixels.get(ident)
        cb = callbacks.get(ident)
        own = captures.get((reg['b'], reg['c'], reg['f']), []) if reg else []
        own = [c for c in own if reg and c['time_ns'] <= reg['time_ns']]
        producer = own[0] if len(own) == 1 else None
        latest = max((s for s in submissions if s['renderer'] == read['b'] and
                      s['image'] == read['d'] and s['time'] < read['time_ns']),
                     key=lambda s: s['time'], default=None)
        newer = bool(latest and reg and latest['serial'] > reg['c'])
        statuses = fence_samples[ident]
        before = statuses.get((0, latest['serial'], latest['fence'])) if latest else None
        after = statuses.get((1, latest['serial'], latest['fence'])) if latest else None
        observations.append({'id': ident, 'time_ns': read['time_ns'],
            'producer_serial': reg['c'] if reg else None, 'compositor_frame': reg['h'] if reg else None,
            'read_counter': cb['e'] if cb else None, 'selected_image': read['d'],
            'producer_image': producer['h'] if producer else None,
            'selection_mismatch': producer['h'] != read['d'] if producer else None,
            'latest_writer_serial': latest['serial'] if latest else None,
            'newer_writer_submitted': newer,
            'newer_writer_complete_before_copy': bool(newer and before == 0),
            'newer_writer_fence_before': before, 'newer_writer_fence_after': after,
            'callback_data_changed': bool(cb['i']) if cb else None,
            'signature': pix['b'] if pix and pix['c'] else None,
            'get_pixels_total_ns': pix['d'] if pix else None,
            'get_pixels_original_ns': pix['e'] if pix else None})
    repeats = []
    for i, o in enumerate(observations):
        if i >= 2 and o['signature'] is not None and o['signature'] == observations[i-2]['signature'] and o['signature'] != observations[i-1]['signature']:
            repeats.append([observations[i-2]['id'], observations[i-1]['id'], o['id']])
    warnings = []
    if dropped:
        warnings.append('Dropped events: correlation may be incomplete.')
    if not events['backend']:
        warnings.append('No Vulkan backend observed; this trace cannot test the Vulkan hypothesis.')
    if any(r['event'] == 'backend' and not (r['b'] and r['c'] and r['e']) for r in rows):
        warnings.append('A submit/wait hook or fence status query is unavailable.')
    if events['context_overflow']:
        warnings.append('Some recorder callbacks could not be instrumented.')
    if not observations:
        warnings.append('No instrumented recorder copies found.')
    if any(o['producer_image'] is None for o in observations):
        warnings.append('Some producer captures are missing or ambiguous; typically the first frame starts before tracing is armed.')
    if any(o['callback_data_changed'] for o in observations):
        warnings.append('Original callback data changed after registration; inspect those rows before assigning producers.')
    return {'event_counts': dict(events), 'dropped': dropped, 'warnings': warnings,
            'copies': len(observations), 'aba_examples': repeats,
            'copies_after_newer_submit': sum(o['newer_writer_submitted'] for o in observations),
            'copies_after_newer_completion': sum(o['newer_writer_complete_before_copy'] for o in observations),
            'selection_mismatches': sum(o['selection_mismatch'] is True for o in observations),
            'observations': observations,
            'interpretation': 'Submission-before-copy establishes reuse ordering. A signaled newer-writer fence establishes that newer submission completed before the copy. Unsignaled fences do not identify which image generation was copied. Sampled A/B/A signatures require moving-scene/video corroboration; this analyzer does not declare a root cause automatically.'}


def analyze(rows, dropped=0):
    result=analyze_observation(rows,dropped)
    rows=sorted(rows,key=lambda r:r['time_ns'])
    by_event=defaultdict(list)
    by_id=defaultdict(dict)
    errors=[]
    unique=('producer_submit','snapshot_begin','snapshot_end','snapshot_wait','delivery_source','pixels','callback_begin','callback_end')
    for row in rows:
        by_event[row['event']].append(row)
        if row['event'] in unique:
            if row['a'] in by_id[row['event']]: errors.append(f"Duplicate {row['event']} for callback {row['a']}")
            by_id[row['event']][row['a']]=row
    writers=[]
    for capture in by_event['capture']:
        for start in by_event['submit_begin']:
            if (capture['a'],capture['b'],capture['e'])==(start['a'],start['b'],start['f']) and capture['time_ns']<=start['time_ns']:
                writers.append((start['time_ns'],capture['a'],capture['h'],capture['b']))
    checks=[]
    for ident,saved in by_id['snapshot_end'].items():
        producer=by_id['producer_submit'].get(ident)
        starts=[r for r in by_event['submit_begin'] if producer and
                (r['a'],r['b'],r['c'],r['e'],r['f'])==(producer['f'],producer['b'],producer['d'],producer['c'],producer['e']) and
                r['time_ns']<producer['time_ns']]
        submitted=any(end['f']==0 and
            (start['a'],start['b'],start['c'],start['d'],start['e'])==(end['a'],end['b'],end['c'],end['d'],end['e']) and
            start['time_ns']<end['time_ns']<producer['time_ns']
            for start in starts for end in by_event['submit_end'])
        begin=by_id['snapshot_begin'].get(ident)
        wait=by_id['snapshot_wait'].get(ident)
        valid=bool(submitted and producer and begin and wait and wait['d']==0 and
            producer['b']==saved['c']==begin['c']==wait['b'] and producer['c']==begin['f']==wait['c'] and
            producer['f']==saved['b']==begin['b'] and saved['d']==begin['e'] and saved['e']==begin['g'] and
            producer['time_ns']<wait['time_ns']<begin['time_ns']<saved['time_ns'])
        reuse=min((time for time,r,image,serial in writers if producer and r==saved['b'] and image==saved['d'] and
                   serial!=saved['c'] and time>producer['time_ns']),default=None)
        before_reuse=bool(reuse is None or saved['time_ns']<reuse)
        checks.append({'id':ident,'valid_producer_and_wait':valid,'saved_before_reuse':before_reuse,'next_reuse_ns':reuse})
        if not valid: errors.append(f'Invalid producer/fence/snapshot chain for callback {ident}')
        if not before_reuse: errors.append(f'Image reused before snapshot finished for callback {ident}')
    signature_matches=0
    for ident,source in by_id['delivery_source'].items():
        saved=by_id['snapshot_end'].get(ident)
        pixels=by_id['pixels'].get(ident)
        valid=bool(source['b']==1 and saved and pixels and pixels['c']==1 and
            source['d']==saved['d'] and source['g']==saved['e'] and source['h']==saved['g']==pixels['b'] and
            saved['time_ns']<source['time_ns']<pixels['time_ns'])
        if valid: signature_matches+=1
        else: errors.append(f'Unprotected or mismatched pixel delivery for callback {ident}')
    for ident in by_id['pixels']:
        if ident not in by_id['delivery_source']: errors.append(f'Missing delivery source for callback {ident}')
    for ident,end in by_id['callback_end'].items():
        begin=by_id['callback_begin'].get(ident)
        if not begin or end['c']!=begin['e']+1: errors.append(f'Unexpected read counter advance for callback {ident}')
    failures=('snapshot_failed','delivery_fallback','context_overflow','duplicate_callback','capture_missing','producer_submit_failed','install_failed','dispatch_conflict','profile_rejected')
    for event in failures:
        if by_event[event]: errors.append(f'{event}: {len(by_event[event])}')
    ready=bool(by_event['installed'] and by_event['installed'][0]['a']==5 and by_event['backend'] and
               all(r['b'] and r['c'] and r['e'] for r in by_event['backend']))
    if not ready: errors.append('Missing successful setup/backend evidence')
    if dropped: errors.append(f'Dropped trace events: {dropped}')
    if not by_event['delivery_source']: errors.append('No measured pixel deliveries')
    def timings(event,field):
        values=sorted(r[field]/1e6 for r in by_event[event])
        return {'mean_ms':sum(values)/len(values),'p95_ms':values[int((len(values)-1)*0.95)],'max_ms':values[-1]} if values else None
    result['fix']={
        'ownership_checks_passed':not errors,
        'errors':errors,'saved_snapshots':len(checks),
        'snapshots_after_reuse':sum(not c['saved_before_reuse'] for c in checks),
        'saved_deliveries':sum(r['b']==1 for r in by_event['delivery_source']),
        'snapshot_delivery_signature_matches':signature_matches,
        'snapshots_unused':len(by_event['snapshot_unused']),
        'fallback_deliveries':len(by_event['delivery_fallback']),
        'snapshot_wait':timings('snapshot_wait','e'),'snapshot_total':timings('snapshot_end','f'),
        'peak_staging_bytes':max((r['h'] for r in by_event['snapshot_end']),default=0),
        'snapshot_checks':checks,
        'interpretation':'Legacy copies_after_newer_* counts describe callback delivery time, not the GPU-to-owned-buffer copy. Protected deliveries may occur after reuse. The fix checks the earlier snapshot, its producer fence, ordering before reuse, and matching saved/delivered signatures. Passing trace checks still requires moving-scene video and stop/restart validation.'
    }
    return result


def read_trace(path):
    lines = Path(path).read_text().splitlines()
    dropped = max([int(s.split('=', 1)[1]) for s in lines if s.startswith('# dropped=')] or [0])
    rows = []
    for row in csv.DictReader(s for s in lines if s and not s.startswith('#')):
        if None in row or None in row.values():
            raise ValueError('Incomplete CSV row; wait for trace flush before copying the file')
        rows.append({k: v if k == 'event' else int(v) for k, v in row.items()})
    return rows, dropped


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Correlate native recorder capture, submission, callback and pixel events.')
    parser.add_argument('trace')
    parser.add_argument('--output')
    args = parser.parse_args()
    rows, dropped = read_trace(args.trace)
    result = analyze(rows, dropped)
    target = Path(args.output) if args.output else Path(args.trace).with_suffix('.analysis.json')
    target.write_text(json.dumps(result, indent=2))
    print(json.dumps({k:v for k,v in result.items() if k!='observations'}, indent=2))
    print('Detailed output:', target)
