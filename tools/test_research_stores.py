import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from game_addresses import ADDRESS
from game_versions import BASE_ID
from research_instrument import instrument, prepare
from research_stores import comparison, manifest_read, validate


class StoreInstrument(unittest.TestCase):
    def test_selection_and_literal_safety(self):
        page = ADDRESS.RESEARCH_RNG_STATE & ~4095
        text = ('void f_7000000100(Cpu *c) {\n'
                f' c->x[0]=0x{page:x}ull;\n'
                ' // ST32(fake, fake)\n const char *s="ST64(fake, fake)";\n'
                ' ST32(c->x[0],1); ST32(c->x[0]+4,2);\n}\n'
                'void f_7000000200(Cpu *c) { ST32(c->x[0],3); }\n')
        sites, functions = [], []
        output = instrument(text, 'main/main_000.c', page, sites, functions)
        self.assertEqual(len(sites), 2)
        self.assertEqual([s['line'] for s in sites], [5, 5])
        self.assertEqual(len(functions), 1)
        self.assertIn('// ST32(fake, fake)', output)
        self.assertIn('"ST64(fake, fake)"', output)
        self.assertIn('f_7000000200(Cpu *c) { ST32(c->x[0],3); }', output)
        self.assertIn('research_probe_st32(c, 1ull, 0x7000000100ull, ', output)

    def test_separate_tree_and_manifest_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);source=root/'generated';game=root/'game';output=root/'diagnostic'
            (source/'main').mkdir(parents=True);(source/'sdk').mkdir();(game/'exefs').mkdir(parents=True)
            raw=bytearray(0x60);raw[:4]=b'NSO0';raw[0x40:0x54]=bytes.fromhex(BASE_ID);(game/'exefs/main').write_bytes(raw)
            (source/'main/main_table.c').write_text(f'const char main_build_id[] = "{BASE_ID}";\n')
            path=source/'main/main_000.c'
            path.write_text(f'void f_7000000100(Cpu *c) {{\n c->x[0]=0x{ADDRESS.RESEARCH_RNG_STATE & ~4095:x}ull;\n ST32(c->x[0],1);\n}}\n')
            before=path.read_bytes();result=prepare(source,game,output)
            self.assertEqual(path.read_bytes(), before)
            manifest,identity=manifest_read(output/'research-stores.json')
            self.assertEqual(result['manifest'], identity)
            self.assertIn(identity, (output/'main/main_table.c').read_text())
            self.assertFalse(manifest['complete_coverage'])
            with self.assertRaises(ValueError):prepare(source,game,output)
            with self.assertRaises(ValueError):prepare(output,game,root/'second')


class StoreAnalysis(unittest.TestCase):
    def fixture(self, values=(9,)):
        first=(1,2,3,4);current=first;rows=[]
        for i,value in enumerate(values):
            after=(value,*current[1:])
            row=dict(write=str(i),begin=str(2+2*i),end=str(3+2*i),elapsed_ns=str(2+i),site='1',function=str(0x7000000100),
                     caller='0',address=str(ADDRESS.RESEARCH_RNG_STATE),size='4',window_update='0',update='0',thread='7')
            row.update({'before'+str(k):str(v) for k,v in enumerate(current)})
            row.update({'after'+str(k):str(v) for k,v in enumerate(after)})
            rows.append(row);current=after
        bounds=[]
        for seq,before,state in ((1,'1',first),(2*len(rows)+2,'0',current)):
            bounds.append(dict(sequence=str(seq),elapsed_ns='1',update='0',thread='7',before=before,
                               **{'rng'+str(k):str(v) for k,v in enumerate(state)}))
        update=dict(rng_before_observed='1',rng_observed='1',**{'rng_before'+str(k):str(v) for k,v in enumerate(first)},
                    **{'rng'+str(k):str(v) for k,v in enumerate(current)})
        identity='a'*64
        meta=dict(format='1',manifest=identity,first_update='0',end_update='1',capacity='8',recorded=str(len(rows)),
                  dropped='0',pending='0',overlaps='0',outside_update='0',invalid='0',recording_complete='1',window_opened='1',window_closed='1')
        sites=[dict(id=i,function='0x7000000100',file='main/main_000.c',line=20+i,bits=32) for i in (1,2)]
        manifest=dict(target=hex(ADDRESS.RESEARCH_RNG_STATE),sites=sites,coverage='static_page_reference_functions')
        return meta,rows,bounds,manifest,identity,[update]

    def trace(self, fixture):
        report,rows,bounds=validate(*fixture)
        return report,rows,bounds,fixture[3]

    def test_complete_partial_scope(self):
        trace=self.trace(self.fixture())
        self.assertTrue(trace[0]['observed_recording_complete'])
        self.assertFalse(trace[0]['complete_coverage'])
        self.assertEqual(trace[0]['reconciled_updates'],1)
        report=comparison(trace,trace)
        self.assertEqual(report['verdict'],'observed_writes_match')
        self.assertEqual(report['consumption_reconstruction'],'not_established')

    def test_first_site_difference(self):
        left=self.fixture();right=copy.deepcopy(left);right[1][0]['site']='2'
        report=comparison(self.trace(left),self.trace(right))
        self.assertEqual(report['verdict'],'observed_writes_differ')
        self.assertEqual(report['first_observed_write_difference']['right']['source_line'],22)
        self.assertEqual(report['first_observed_write_difference']['differing_fields'],['site'])

    def test_extra_noop_write_is_not_extra_random_call(self):
        report=comparison(self.trace(self.fixture()),self.trace(self.fixture((9,9))))
        self.assertEqual(report['first_observed_write_difference']['write_in_update'],1)
        self.assertIsNone(report['first_observed_write_difference']['left'])
        self.assertEqual(report['first_observed_write_difference']['differing_fields'],['write_missing'])
        self.assertEqual(report['consumption_reconstruction'],'not_established')

    def test_state_chain_gap(self):
        fixture=self.fixture((9,10));fixture[1][1]['before0']='88'
        report=self.trace(fixture)[0]
        self.assertFalse(report['observed_recording_complete'])
        self.assertEqual(report['first_state_chain_gap_updates'],[0])

    def test_missing_capture_cannot_match(self):
        fixture=self.fixture();fixture[0]['dropped']='1';fixture[0]['recording_complete']='0'
        report=comparison(self.trace(fixture),self.trace(fixture))
        self.assertEqual(report['verdict'],'incomplete')

    def test_overlap_and_pending(self):
        fixture=self.fixture((9,10));fixture[0]['overlaps']='1';fixture[1][0]['end']='5';fixture[1][1]['begin']='3';fixture[1][1]['end']='4'
        self.assertFalse(self.trace(fixture)[0]['observed_recording_complete'])
        fixture=self.fixture();fixture[0]['pending']='1';fixture[1][0]['end']='0'
        self.assertIn('unpaired_write',self.trace(fixture)[0]['problems'])

    def test_changes_outside_store_and_wrong_boundary(self):
        fixture=self.fixture();fixture[1][0]['after1']='99'
        self.assertIn('changes_outside_recorded_write',self.trace(fixture)[0]['problems'])
        fixture=self.fixture();fixture[5][0]['rng0']='99'
        self.assertIn('boundary_differs_from_update_sample',self.trace(fixture)[0]['problems'])

    def test_receipt_site_identity_and_sequence_limits(self):
        for key,value in (('site','999'),('function','123'),('begin',str(2**63)),('address','0')):
            fixture=self.fixture();fixture[1][0][key]=value
            with self.assertRaises(ValueError):validate(*fixture)
        fixture=self.fixture();fixture[0]['manifest']='b'*64
        with self.assertRaises(ValueError):validate(*fixture)
        fixture=self.fixture();fixture[0]['recorded']='2'
        with self.assertRaises(ValueError):validate(*fixture)


if __name__=='__main__':unittest.main()
