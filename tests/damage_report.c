#include <dix-config.h>

#include "misc.h"
#include "scrnintstr.h"
#include "regionstr.h"
#include "damagestr.h"

static int report_count;
static BoxRec last_extents;

static void
record_damage(DamagePtr damage, RegionPtr region, void *closure)
{
    int *expected_closure = closure;

    if (damage == NULL || expected_closure == NULL)
        return;
    ++report_count;
    last_extents = *RegionExtents(region);
}

int
main(void)
{
    int closure = 17;
    DamageRec damage = {
        .damageLevel = DamageReportDeltaRegion,
        .damageReport = record_damage,
        .closure = &closure,
    };
    BoxRec first_box = { 2, 3, 11, 13 };
    BoxRec second_box = { 8, 9, 16, 18 };
    RegionRec first;
    RegionRec second;

    RegionNull(&damage.damage);
    RegionNull(&damage.pendingDamage);
    RegionInit(&first, &first_box, 1);
    RegionInit(&second, &second_box, 1);

    DamageReportDamage(&damage, &first);
    if (report_count != 1 || !RegionNotEmpty(DamageRegion(&damage)) ||
        last_extents.x1 != first_box.x1 || last_extents.y1 != first_box.y1 ||
        last_extents.x2 != first_box.x2 || last_extents.y2 != first_box.y2) {
        return 1;
    }

    /* Delta reporting must suppress an already-accounted-for region. */
    DamageReportDamage(&damage, &first);
    if (report_count != 1)
        return 2;

    DamageReportDamage(&damage, &second);
    if (report_count != 2)
        return 3;
    if (RegionExtents(DamageRegion(&damage))->x2 != second_box.x2 ||
        RegionExtents(DamageRegion(&damage))->y2 != second_box.y2) {
        return 4;
    }

    DamageEmpty(&damage);
    if (RegionNotEmpty(DamageRegion(&damage)))
        return 5;
    DamageReportDamage(&damage, &first);
    if (report_count != 3)
        return 6;

    RegionUninit(&second);
    RegionUninit(&first);
    RegionUninit(&damage.pendingDamage);
    RegionUninit(&damage.damage);
    return 0;
}
