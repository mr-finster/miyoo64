"""Asset-free checks for relocated PAL overlays and the actual native DMA callbacks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import gen_build
import test_gen_build

class OverlayClassificationTests(unittest.TestCase):
    def segment(self,path,name='relocated',klass='fixed',region='pal'):
        return {'name':name,'class':klass,'objs':[f'ver/{region}/build/{path}.o']}
    def test_relocated_pal_overlay_objects(self):
        for p in ['world/area_jan/jan_02/main','world/area_hos/hos_05/intro',
                  'battle/move/jump/power_bounce','battle/move/star_power/star_beam',
                  'battle/move/star_power/peach_beam','battle/area/kzn2/actor/lava_piranha',
                  'battle/action_cmd/jump','world/model_anim/kzn/01','world/partner/goombario']:
            with self.subTest(source=p):
                self.assertTrue(gen_build.is_overlay(self.segment('src/'+p+'.c'),'pal'))
    def test_resident_and_graphics_objects_are_not_overlays(self):
        for p in ['src/main.c','src/sprite.c','src/entity/SaveBlock.c','src/effects/gfx/peach_star_beam.c','assets/pal/logo.c']:
            with self.subTest(source=p):self.assertFalse(gen_build.is_overlay(self.segment(p),'pal'))
    def test_fixed_menus_and_followed_map(self):
        for n in ['filemenu','pause','battle_code']:
            self.assertTrue(gen_build.is_overlay(self.segment('src/'+n+'.c',n),'pal'))
        self.assertTrue(gen_build.is_overlay(self.segment('src/world/area_kmr/kmr_00/main.c',klass='follow'),'pal'))
    def test_region_prefix_is_checked(self):
        with self.assertRaises(ValueError):gen_build.is_overlay(self.segment('src/main.c',region='us'),'pal')
    def test_us_current_window_contract_is_retained(self):
        for cls in gen_build.OVL_CLASSES:
            self.assertTrue(gen_build.is_overlay(self.segment('src/fake.c',klass=cls,region='us')))
        self.assertFalse(gen_build.is_overlay(self.segment('src/main.c',region='us')))
    def test_pal_make_linker_and_table_agree(self):
        t=test_gen_build.BuildGeneratorTests();t.setUp()
        try:
            old='src/world/map.c';new='src/world/area_kmr/kmr_00/main.c'
            t.write('refs/papermario/'+new,'/* synthetic code */\n')
            for p in [t.decomp/'build.ninja',t.decomp/'ver/pal/papermario.ld']:
                p.write_text(p.read_text().replace(old,new))
            for platform in ['pc','handheld','miyoo']:
                p=t.cli(platform,'--region','pal');self.assertEqual(p.returncode,0,p.stderr)
                ld=(t.build/'pal/assets_noload.ld').read_text()
                self.assertIn('*/src/world/area_kmr/kmr_00/main.c.o(.assetdata .assetdata.*)',ld)
                self.assertNotIn('(.bss',ld)
                self.assertIn('__pm64ovl_map_start',(t.build/'pal/port_segtab.c').read_text())
                if platform!='miyoo':self.assertIn('main.c.o: CFLAGS += -fno-zero-initialized-in-bss',(t.build/f'pal/{platform}_game.mk').read_text())
        finally:t.doCleanups()

class OverlayRuntimeTests(unittest.TestCase):
    def test_real_callbacks_repeated_loads_guards_and_preserved_bss(self):
        root=Path(__file__).resolve().parents[2]
        source=(root/'game/port/port_segments.c').read_text();bodies=source[source.index('int port_code_segment('):]
        prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
uint8_t __assets_start[64];
unsigned port_frame;
static int calls,fail_read,bss_sentinel=77;
static const unsigned char pristine[8]={1,2,3,4,5,6,7,8};
const struct {uint32_t start,end;const char *name;uint8_t *data,*data_end;} port_code_segs[]={
 {100,200,"map",__assets_start+16,__assets_start+24},
 {200,300,"resident",0,0},{300,400,"empty",__assets_start+32,__assets_start+32}};
const int port_code_segs_n=3;
void port_log(const char *s,...){(void)s;}
int port_state_pristine_bytes(uint32_t offset,void *dst,uint32_t n){
 assert(offset==16 && n==8);calls++;
 if(fail_read)return 0;
 memcpy(dst,pristine,n);return 1;
}
'''
        suffix=r'''
int main(int argc,char **argv){
 int seg=-1;
 assert(port_code_segment(100,200,&seg)==1 && seg==0);
 assert(port_code_segment(199,200,0)==1);
 assert(port_code_segment(99,100,0)==0);assert(port_code_segment(400,401,0)==0);
 port_segment_loaded(-1,100);port_segment_loaded(3,100);port_segment_loaded(0,101);
 port_segment_loaded(1,200);port_segment_loaded(2,300);assert(calls==0);
 port_segment_loaded(0,100);
 if(argc>1){assert(calls==0);return 0;}
 assert(calls==1 && memcmp(__assets_start+16,pristine,8)==0);
 memset(__assets_start+16,0xcc,8);port_segment_loaded(0,100);
 assert(calls==2 && memcmp(__assets_start+16,pristine,8)==0);
 fail_read=1;port_segment_loaded(0,100);assert(calls==3);
 assert(bss_sentinel==77);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='pal-overlay-') as tmp:
            p=Path(tmp);(p/'test.c').write_text(prefix+bodies+suffix)
            cmd=['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined,bounds','-fno-sanitize-recover=all','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')]
            subprocess.run(cmd,check=True,capture_output=True)
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
            env.pop('PM64_OVLRESET',None);env.pop('PM64_OVLLOG',None)
            subprocess.run([str(p/'test')],check=True,env=env,capture_output=True)
            env['PM64_OVLRESET']='0';subprocess.run([str(p/'test'),'disabled'],check=True,env=env,capture_output=True)
if __name__=='__main__':unittest.main()
