# SPDX-License-Identifier: MIT

from .operators import CLASSES as OPERATOR_CLASSES
from .properties import CLASSES as PROPERTY_CLASSES, register_properties, unregister_properties
from .ui import CLASSES as UI_CLASSES

CLASSES = PROPERTY_CLASSES + OPERATOR_CLASSES + UI_CLASSES


def register():
    import bpy

    for cls in CLASSES:
        bpy.utils.register_class(cls)
    register_properties()


def unregister():
    import bpy

    shutdown_native()
    unregister_properties()
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


def shutdown_native():
    try:
        from .native_bridge import shutdown

        shutdown()
    except Exception:
        pass
