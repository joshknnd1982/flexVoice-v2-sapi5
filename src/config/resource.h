#pragma once

#define IDD_CONFIG            101
#define IDI_APPICON           102

// Voice selection
#define IDC_BASEVOICE         1001
#define IDC_LANGUAGE          1002
#define IDC_SAMPLERATE        1003

// One edit + spin pair per speech parameter. Kept as contiguous ranges so the
// dialog code can walk them with a loop instead of a switch.
#define IDC_PARAM_EDIT_BASE   1100    // 1100 .. 1100 + FVP_COUNT - 1
#define IDC_PARAM_SPIN_BASE   1200    // 1200 .. 1200 + FVP_COUNT - 1
#define IDC_PARAM_LABEL_BASE  1300    // 1300 .. 1300 + FVP_COUNT - 1

// Buttons and options
#define IDC_PREVIEW           1400
#define IDC_STOP              1401
#define IDC_DEFAULTS          1402
#define IDC_DEBUGLOG          1403
#define IDC_PREVIEWTEXT       1404
#define IDC_STATUS            1405
#define IDC_TESTVOICE         1406
