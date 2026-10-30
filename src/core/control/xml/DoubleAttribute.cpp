#include "DoubleAttribute.h"

#include <string>  // for allocator, string

#include <glib.h>  // for g_ascii_formatd, G_ASCII_DTOSTR_B...

#include "control/xml/Attribute.h"  // for XMLAttribute
#include "util/OutputStream.h"      // for OutputStream
#include "util/Util.h"              // for PRECISION_FORMAT_STRING

DoubleAttribute::DoubleAttribute(const char8_t* name, double value): XMLAttribute(name) { this->value = value; }

DoubleAttribute::~DoubleAttribute() = default;

void DoubleAttribute::writeOut(OutputStream* out) {
    Util::writeDouble(out, value);
}
