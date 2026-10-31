/* banjo.ld: added to the default link (INSERT). Each overlay's .data and .bss (hh/Makefile.handheld renames them
 * per overlay) in a block of its own, with start/end symbols, so hh_bk.c can put an overlay's variables back as the
 * N64's overlay manager does each time it loads that overlay. */
SECTIONS {
  .bk_ovl_data : ALIGN(16) {
    hh_ovl_core2_data_start = .; KEEP(*(.ovl_core2_data)) . = ALIGN(16); hh_ovl_core2_data_end = .;
    hh_ovl_emptyLvl_data_start = .; KEEP(*(.ovl_emptyLvl_data)) . = ALIGN(16); hh_ovl_emptyLvl_data_end = .;
    hh_ovl_CC_data_start = .; KEEP(*(.ovl_CC_data)) . = ALIGN(16); hh_ovl_CC_data_end = .;
    hh_ovl_MMM_data_start = .; KEEP(*(.ovl_MMM_data)) . = ALIGN(16); hh_ovl_MMM_data_end = .;
    hh_ovl_GV_data_start = .; KEEP(*(.ovl_GV_data)) . = ALIGN(16); hh_ovl_GV_data_end = .;
    hh_ovl_TTC_data_start = .; KEEP(*(.ovl_TTC_data)) . = ALIGN(16); hh_ovl_TTC_data_end = .;
    hh_ovl_MM_data_start = .; KEEP(*(.ovl_MM_data)) . = ALIGN(16); hh_ovl_MM_data_end = .;
    hh_ovl_BGS_data_start = .; KEEP(*(.ovl_BGS_data)) . = ALIGN(16); hh_ovl_BGS_data_end = .;
    hh_ovl_RBB_data_start = .; KEEP(*(.ovl_RBB_data)) . = ALIGN(16); hh_ovl_RBB_data_end = .;
    hh_ovl_FP_data_start = .; KEEP(*(.ovl_FP_data)) . = ALIGN(16); hh_ovl_FP_data_end = .;
    hh_ovl_CCW_data_start = .; KEEP(*(.ovl_CCW_data)) . = ALIGN(16); hh_ovl_CCW_data_end = .;
    hh_ovl_SM_data_start = .; KEEP(*(.ovl_SM_data)) . = ALIGN(16); hh_ovl_SM_data_end = .;
    hh_ovl_cutscenes_data_start = .; KEEP(*(.ovl_cutscenes_data)) . = ALIGN(16); hh_ovl_cutscenes_data_end = .;
    hh_ovl_lair_data_start = .; KEEP(*(.ovl_lair_data)) . = ALIGN(16); hh_ovl_lair_data_end = .;
    hh_ovl_fight_data_start = .; KEEP(*(.ovl_fight_data)) . = ALIGN(16); hh_ovl_fight_data_end = .;
  }
} INSERT AFTER .data;
SECTIONS {
  .bk_ovl_bss (NOLOAD) : ALIGN(16) {
    hh_ovl_core2_bss_start = .; *(.ovl_core2_bss) . = ALIGN(16); hh_ovl_core2_bss_end = .;
    hh_ovl_emptyLvl_bss_start = .; *(.ovl_emptyLvl_bss) . = ALIGN(16); hh_ovl_emptyLvl_bss_end = .;
    hh_ovl_CC_bss_start = .; *(.ovl_CC_bss) . = ALIGN(16); hh_ovl_CC_bss_end = .;
    hh_ovl_MMM_bss_start = .; *(.ovl_MMM_bss) . = ALIGN(16); hh_ovl_MMM_bss_end = .;
    hh_ovl_GV_bss_start = .; *(.ovl_GV_bss) . = ALIGN(16); hh_ovl_GV_bss_end = .;
    hh_ovl_TTC_bss_start = .; *(.ovl_TTC_bss) . = ALIGN(16); hh_ovl_TTC_bss_end = .;
    hh_ovl_MM_bss_start = .; *(.ovl_MM_bss) . = ALIGN(16); hh_ovl_MM_bss_end = .;
    hh_ovl_BGS_bss_start = .; *(.ovl_BGS_bss) . = ALIGN(16); hh_ovl_BGS_bss_end = .;
    hh_ovl_RBB_bss_start = .; *(.ovl_RBB_bss) . = ALIGN(16); hh_ovl_RBB_bss_end = .;
    hh_ovl_FP_bss_start = .; *(.ovl_FP_bss) . = ALIGN(16); hh_ovl_FP_bss_end = .;
    hh_ovl_CCW_bss_start = .; *(.ovl_CCW_bss) . = ALIGN(16); hh_ovl_CCW_bss_end = .;
    hh_ovl_SM_bss_start = .; *(.ovl_SM_bss) . = ALIGN(16); hh_ovl_SM_bss_end = .;
    hh_ovl_cutscenes_bss_start = .; *(.ovl_cutscenes_bss) . = ALIGN(16); hh_ovl_cutscenes_bss_end = .;
    hh_ovl_lair_bss_start = .; *(.ovl_lair_bss) . = ALIGN(16); hh_ovl_lair_bss_end = .;
    hh_ovl_fight_bss_start = .; *(.ovl_fight_bss) . = ALIGN(16); hh_ovl_fight_bss_end = .;
  }
} INSERT AFTER .bss;
