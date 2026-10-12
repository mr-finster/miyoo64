#include <ultra64.h>

#include <bondtypes.h>
#include "chrai.h"
#include "propobj.h"
#include "cleanup_objects.h"
#include "loadobjectmodel.h"


#ifdef PORT
/* The N64 reads a propDef's type as the low byte of its header word; here the header keeps
 * its struct layout ([u16 extrascale][u8 state][u8 type]), so read the field. */
#define HH_PDEF_TYPE(p) (((PropDefHeaderRecord *) (p))->type)
#else
#define HH_PDEF_TYPE(p) ((u8) (p)[0])
#endif
void cleanupObjects(s32 stage)
{
    u32 *obj = (u32)g_CurrentSetup.propDefs;
    
    if (obj)
    {
        while (HH_PDEF_TYPE(obj) != PROPDEF_END)
        {
            switch (HH_PDEF_TYPE(obj))
            {
                case PROPDEF_DOOR:
                case PROPDEF_PROP:
                case PROPDEF_KEY:
                case PROPDEF_ALARM:
                case PROPDEF_CCTV:
                case PROPDEF_MAGAZINE:
                case PROPDEF_COLLECTABLE:
                case PROPDEF_MONITOR:
                case PROPDEF_MULTI_MONITOR:
                case PROPDEF_RACK:
                case PROPDEF_AUTOGUN:
                case PROPDEF_HAT:
                case PROPDEF_AMMO:
                case PROPDEF_ARMOUR:
                case PROPDEF_GAS_RELEASING:
                case PROPDEF_VEHICHLE:
                case PROPDEF_AIRCRAFT:
                case PROPDEF_UNK41:
                case PROPDEF_GLASS:
                case PROPDEF_SAFE:
                case PROPDEF_TANK:
                case PROPDEF_TINTED_GLASS:
                    objFreePermanently((ObjectRecord *)obj, 1);
                    break;
                case PROPDEF_DOOR_SCALE:
                case PROPDEF_GUARD:
                case PROPDEF_LINK:
                case PROPDEF_DEBRIS:
                case PROPDEF_UNK16:
                case PROPDEF_GUARD_ATTRIBUTE:
                case PROPDEF_SWITCH:
                case PROPDEF_TAG:
                case PROPDEF_OBJECTIVE_START:
                case PROPDEF_OBJECTIVE_END:
                case PROPDEF_OBJECTIVE_DESTROY_OBJECT:
                case PROPDEF_OBJECTIVE_COMPLETE_CONDITION:
                case PROPDEF_OBJECTIVE_FAIL_CONDITION:
                case PROPDEF_OBJECTIVE_COLLECT_OBJECT:
                case PROPDEF_OBJECTIVE_DEPOSIT_OBJECT:
                case PROPDEF_OBJECTIVE_PHOTOGRAPH:
                case PROPDEF_OBJECTIVE_NULL:
                case PROPDEF_OBJECTIVE_ENTER_ROOM:
                case PROPDEF_OBJECTIVE_DEPOSIT_OBJECT_IN_ROOM:
                case PROPDEF_OBJECTIVE_COPY_ITEM:
                case PROPDEF_WATCH_MENU_OBJECTIVE_TEXT:
                case PROPDEF_RENAME:
                case PROPDEF_LOCK_DOOR:
                case PROPDEF_SAFE_ITEM:
                case PROPDEF_CAMERAPOS:
                
                    break;
                
            }

            obj =  obj + sizepropdef(obj) ;
        }
    }
}
