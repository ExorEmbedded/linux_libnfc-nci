/******************************************************************************
 *
 *  Copyright (C) 2015 NXP Semiconductors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License")
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "linux_nfc_api.h"
#include "tools.h"
typedef enum eDevState
{
    eDevState_NONE,
    eDevState_WAIT_ARRIVAL,
    eDevState_PRESENT,
    eDevState_WAIT_DEPARTURE,
    eDevState_DEPARTED,
    eDevState_EXIT
}eDevState;
typedef enum eSnepClientState
{
    eSnepClientState_WAIT_OFF,
    eSnepClientState_OFF,
    eSnepClientState_WAIT_READY,
    eSnepClientState_READY,
    eSnepClientState_EXIT
}eSnepClientState;
typedef enum eHCEState
{
    eHCEState_NONE,
    eHCEState_WAIT_DATA,
    eHCEState_DATA_RECEIVED,
    eHCEState_EXIT
}eHCEState;
typedef enum eDevType
{
    eDevType_NONE,
    eDevType_TAG,
    eDevType_P2P,
    eDevType_READER
}eDevType;
typedef enum eOutputFormat
{
    eOutputFormat_TXT,
    eOutputFormat_JSON
}eOutputFormat;

typedef enum T4T_NDEF_EMU_state_t
{
    Ready,
    NDEF_Application_Selected,
    CC_Selected,
    NDEF_Selected
} T4T_NDEF_EMU_state_t;

static void* g_ThreadHandle = NULL;
static void* g_devLock = NULL;
static void* g_SnepClientLock = NULL;
static void* g_HCELock = NULL;
static eDevState g_DevState = eDevState_NONE;
static eDevType g_Dev_Type = eDevType_NONE;
static eSnepClientState g_SnepClientState = eSnepClientState_OFF;
static eHCEState g_HCEState = eHCEState_NONE;
static nfc_tag_info_t g_TagInfo;
static nfcTagCallback_t g_TagCB;
static nfcHostCardEmulationCallback_t g_HceCB;
static nfcSnepServerCallback_t g_SnepServerCB;
static nfcSnepClientCallback_t g_SnepClientCB;
unsigned char *HCE_data = NULL;
unsigned int HCE_dataLenght = 0x00;
const unsigned char T4T_NDEF_EMU_APP_Select[] = {0x00,0xA4,0x04,0x00,0x07,0xD2,0x76,0x00,0x00,0x85,0x01,0x01};
const unsigned char T4T_NDEF_EMU_CC[] = {0x00, 0x0F, 0x20, 0x00, 0xFF, 0x00, 0xFF, 0x04, 0x06, 0xE1, 0x04, 0x00, 0xFF, 0x00, 0xFF};
const unsigned char T4T_NDEF_EMU_CC_Select[] = {0x00,0xA4,0x00,0x0C,0x02,0xE1,0x03};
const unsigned char T4T_NDEF_EMU_NDEF_Select[] = {0x00,0xA4,0x00,0x0C,0x02,0xE1,0x04};
const unsigned char T4T_NDEF_EMU_Read[] = {0x00,0xB0};
const unsigned char T4T_NDEF_EMU_OK[] = {0x90, 0x00};
const unsigned char T4T_NDEF_EMU_NOK[] = {0x6A, 0x82};
unsigned char *pT4T_NdefRecord = NULL;
unsigned short T4T_NdefRecord_size = 0;

typedef void T4T_NDEF_EMU_Callback_t (unsigned char*, unsigned short);
static T4T_NDEF_EMU_state_t eT4T_NDEF_EMU_State = Ready;
static T4T_NDEF_EMU_Callback_t *pT4T_NDEF_EMU_PushCb = NULL;
void help(int mode);
int InitEnv();
int LookForTag(char** args, int args_len, char* tag, char** data, int format);

//
// <extensions>
//

#define STR_MAX 128
#define JSON_MAX 2048

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define PRINT_TXT(...) do { if (g_OutputFormat == eOutputFormat_TXT) fprintf(stdout, __VA_ARGS__); } while (0)
//#define PRINT_JSON(...) do { if (g_OutputFormat == eOutputFormat_JSON) fprintf((g_OutputFile ? g_OutputFile : stdout), __VA_ARGS__); } while(0)
#define PRINT_JSON(...) do { \
    if (g_OutputFormat == eOutputFormat_JSON) { \
        if (g_OutputFile) { \
                char buf[STR_MAX]; \
                snprintf(buf, sizeof(buf), __VA_ARGS__); \
                strncat(g_jsonBuf, buf, sizeof(buf)); \
        } else \
                fprintf(stderr, __VA_ARGS__); \
    } \
} while(0)

#ifndef CLOCK_TICK_RATE
#define CLOCK_TICK_RATE 1193180
#endif

#include <fcntl.h>
#include <linux/input.h>
static eOutputFormat g_OutputFormat = eOutputFormat_TXT;
FILE *g_OutputFile = NULL;
char g_jsonBuf[JSON_MAX];
int g_msgCounter = 0;
int g_doBeep = 0;

static void do_beep(int freq) {
    int period = (freq != 0 ? (int)(CLOCK_TICK_RATE/freq) : freq);
    int console_fd = open("/dev/tty0", O_WRONLY);

    struct input_event e;
    e.type = EV_SND;
    e.code = SND_TONE;
    e.value = freq;

    if(write(console_fd, &e, sizeof(struct input_event)) < 0)
    {
        putchar('\a'); /* See above */
        perror("write");
    }

    close(console_fd);
}
//
// </extensions>
//

/********************************** HCE **********************************/
static void T4T_NDEF_EMU_FillRsp (unsigned char *pRsp, unsigned short offset, unsigned char length)
{
    if (offset == 0)
    {
        pRsp[0] = (T4T_NdefRecord_size & 0xFF00) >> 8;
        pRsp[1] = (T4T_NdefRecord_size & 0x00FF);
        memcpy(&pRsp[2], &pT4T_NdefRecord[0], length-2);
    }
    else if (offset == 1)
    {
        pRsp[0] = (T4T_NdefRecord_size & 0x00FF);
        memcpy(&pRsp[1], &pT4T_NdefRecord[0], length-1);
    }
    else
    {
        memcpy(pRsp, &pT4T_NdefRecord[offset-2], length);
    }
    /* Did we reached the end of NDEF record ?*/
    if ((offset + length) >= (T4T_NdefRecord_size + 2))
    {
        /* Notify application of the NDEF send */
        if(pT4T_NDEF_EMU_PushCb != NULL) pT4T_NDEF_EMU_PushCb(pT4T_NdefRecord, T4T_NdefRecord_size);
    }
}
void T4T_NDEF_EMU_SetRecord(unsigned char *pRecord, unsigned short Record_size, T4T_NDEF_EMU_Callback_t *cb)
{
    pT4T_NdefRecord = pRecord;
    T4T_NdefRecord_size = Record_size;
    pT4T_NDEF_EMU_PushCb =  cb;
}
void T4T_NDEF_EMU_Reset(void)
{
    eT4T_NDEF_EMU_State = Ready;
}
void T4T_NDEF_EMU_Next(unsigned char *pCmd, unsigned short Cmd_size, unsigned char *pRsp, unsigned short *pRsp_size)
{
    unsigned char eStatus = 0x00;
    if (!memcmp(pCmd, T4T_NDEF_EMU_APP_Select, sizeof(T4T_NDEF_EMU_APP_Select)))
    {
        *pRsp_size = 0;
        eStatus = 0x01;
        eT4T_NDEF_EMU_State = NDEF_Application_Selected;
    }
    else if (!memcmp(pCmd, T4T_NDEF_EMU_CC_Select, sizeof(T4T_NDEF_EMU_CC_Select)))
    {
        if(eT4T_NDEF_EMU_State == NDEF_Application_Selected)
        {
            *pRsp_size = 0;
            eStatus = 0x01;
            eT4T_NDEF_EMU_State = CC_Selected;
        }
    }
    else if (!memcmp(pCmd, T4T_NDEF_EMU_NDEF_Select, sizeof(T4T_NDEF_EMU_NDEF_Select)))
    {
        *pRsp_size = 0;
        eStatus = 0x01;
        eT4T_NDEF_EMU_State = NDEF_Selected;
    }
    else if (!memcmp(pCmd, T4T_NDEF_EMU_Read, sizeof(T4T_NDEF_EMU_Read)))
    {
        if(eT4T_NDEF_EMU_State == CC_Selected)
        {
            memcpy(pRsp, T4T_NDEF_EMU_CC, sizeof(T4T_NDEF_EMU_CC));
            *pRsp_size = sizeof(T4T_NDEF_EMU_CC);
            eStatus = 0x01;
        }
        else if (eT4T_NDEF_EMU_State == NDEF_Selected)
        {
            unsigned short offset = (pCmd[2] << 8) + pCmd[3];
            unsigned char length = pCmd[4];
            if(length <= (T4T_NdefRecord_size + offset + 2))
            {
                T4T_NDEF_EMU_FillRsp(pRsp, offset, length);
                *pRsp_size = length;
                eStatus = 0x01;
            }
        }
    }
    if (eStatus == 0x01)
    {
        memcpy(&pRsp[*pRsp_size], T4T_NDEF_EMU_OK, sizeof(T4T_NDEF_EMU_OK));
        *pRsp_size += sizeof(T4T_NDEF_EMU_OK);
    } 
    else
    {
        memcpy(pRsp, T4T_NDEF_EMU_NOK, sizeof(T4T_NDEF_EMU_NOK));
        *pRsp_size = sizeof(T4T_NDEF_EMU_NOK);
        T4T_NDEF_EMU_Reset();
    }
}
/********************************** CallBack **********************************/
void onDataReceived(unsigned char *data, unsigned int data_length)
{
    framework_LockMutex(g_HCELock);
    
    HCE_dataLenght = data_length;
    HCE_data = malloc(HCE_dataLenght * sizeof(unsigned char));
    memcpy(HCE_data, data, data_length);
    
    if(eHCEState_NONE == g_HCEState)
    {
        g_HCEState = eHCEState_DATA_RECEIVED;
    }
    else if (eHCEState_WAIT_DATA == g_HCEState)
    {
        g_HCEState = eHCEState_DATA_RECEIVED;
        framework_NotifyMutex(g_HCELock, 0);
    }
    
    framework_UnlockMutex(g_HCELock);
}
void onHostCardEmulationActivated(unsigned char mode)
{
    framework_LockMutex(g_devLock);
    
    T4T_NDEF_EMU_Reset();
    
    if(eDevState_WAIT_ARRIVAL == g_DevState)
    {
        LOG("\tNFC Reader Found, mode=0x%.2x\n\n", mode);
        g_DevState = eDevState_PRESENT;
        g_Dev_Type = eDevType_READER;
        framework_NotifyMutex(g_devLock, 0);
    }
    else if(eDevState_WAIT_DEPARTURE == g_DevState)
    {
        g_DevState = eDevState_PRESENT;
        g_Dev_Type = eDevType_READER;
        framework_NotifyMutex(g_devLock, 0);
    }
    else if(eDevState_EXIT == g_DevState)
    {
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
        framework_NotifyMutex(g_devLock, 0);
    }
    else
    {
        g_DevState = eDevState_PRESENT;
        g_Dev_Type = eDevType_READER;
    }
    framework_UnlockMutex(g_devLock);
}
void onHostCardEmulationDeactivated()
{
    framework_LockMutex(g_devLock);
    
    if(eDevState_WAIT_DEPARTURE == g_DevState)
    {
        LOG("\tNFC Reader Lost\n\n");
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
        framework_NotifyMutex(g_devLock, 0);
    }
    else if(eDevState_PRESENT == g_DevState)
    {
        LOG("\tNFC Reader Lost\n\n");
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
    }
    else if(eDevState_WAIT_ARRIVAL == g_DevState)
    {
    }
    else if(eDevState_EXIT == g_DevState)
    {
    }
    else
    {
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
    }
    framework_UnlockMutex(g_devLock);
    
    framework_LockMutex(g_HCELock);
    if(eHCEState_WAIT_DATA == g_HCEState)
    {
        g_HCEState = eHCEState_NONE;
        framework_NotifyMutex(g_HCELock, 0x00);
    }
    else if(eHCEState_EXIT == g_HCEState)
    {
        
    }
    else
    {
        g_HCEState = eHCEState_NONE;
    }
    framework_UnlockMutex(g_HCELock);
}
 
void onTagArrival(nfc_tag_info_t *pTagInfo)
{
    framework_LockMutex(g_devLock);
    
    if(eDevState_WAIT_ARRIVAL == g_DevState)
    {    
        LOG("\tNFC Tag Found\n\n");

        memcpy(&g_TagInfo, pTagInfo, sizeof(nfc_tag_info_t));
        g_DevState = eDevState_PRESENT;
        g_Dev_Type = eDevType_TAG;
        framework_NotifyMutex(g_devLock, 0);
    }
    else if(eDevState_WAIT_DEPARTURE == g_DevState)
    {    
        memcpy(&g_TagInfo, pTagInfo, sizeof(nfc_tag_info_t));
        g_DevState = eDevState_PRESENT;
        g_Dev_Type = eDevType_TAG;
        framework_NotifyMutex(g_devLock, 0);
    }
    else if(eDevState_EXIT == g_DevState)
    {
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
        framework_NotifyMutex(g_devLock, 0);
    }
    else
    {
        g_DevState = eDevState_PRESENT;
        g_Dev_Type = eDevType_TAG;
    }
    framework_UnlockMutex(g_devLock);
}
void onTagDeparture(void)
{    
    framework_LockMutex(g_devLock);
    
    
    if(eDevState_WAIT_DEPARTURE == g_DevState)
    {
        LOG("\tNFC Tag Lost\n\n");
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
        framework_NotifyMutex(g_devLock, 0);
    }
    else if(eDevState_WAIT_ARRIVAL == g_DevState)
    {
    }    
    else if(eDevState_EXIT == g_DevState)
    {
    }
    else
    {
        g_DevState = eDevState_DEPARTED;
        g_Dev_Type = eDevType_NONE;
    }
    framework_UnlockMutex(g_devLock);
}
void onDeviceArrival (void)
{
    framework_LockMutex(g_devLock);
    
    switch(g_DevState)
    {
        case eDevState_WAIT_DEPARTURE:
        {
            g_DevState = eDevState_PRESENT;
            g_Dev_Type = eDevType_P2P;
            framework_NotifyMutex(g_devLock, 0);
        } break;
        case eDevState_EXIT:
        {
            g_Dev_Type = eDevType_P2P;
        } break;
        case eDevState_NONE:
        {
            g_DevState = eDevState_PRESENT;
            g_Dev_Type = eDevType_P2P;
        } break;
        case eDevState_WAIT_ARRIVAL:
        {
            g_DevState = eDevState_PRESENT;
            g_Dev_Type = eDevType_P2P;
            framework_NotifyMutex(g_devLock, 0);
        } break;
        case eDevState_PRESENT:
        {
            g_Dev_Type = eDevType_P2P;
        } break;
        case eDevState_DEPARTED:
        {
            g_Dev_Type = eDevType_P2P;
            g_DevState = eDevState_PRESENT;
        } break;
    }
    
    framework_UnlockMutex(g_devLock);
}

void onDeviceDeparture (void)
{
    framework_LockMutex(g_devLock);
    
    switch(g_DevState)
    {
        case eDevState_WAIT_DEPARTURE:
        {
            g_DevState = eDevState_DEPARTED;
            g_Dev_Type = eDevType_NONE;
            framework_NotifyMutex(g_devLock, 0);
        } break;
        case eDevState_EXIT:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
        case eDevState_NONE:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
        case eDevState_WAIT_ARRIVAL:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
        case eDevState_PRESENT:
        {
            g_Dev_Type = eDevType_NONE;
            g_DevState = eDevState_DEPARTED;
        } break;
        case eDevState_DEPARTED:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
    }
    framework_UnlockMutex(g_devLock);
    
    framework_LockMutex(g_SnepClientLock);
    
    switch(g_SnepClientState)
    {
        case eSnepClientState_WAIT_OFF:
        {
            g_SnepClientState = eSnepClientState_OFF;
            framework_NotifyMutex(g_SnepClientLock, 0);
        } break;
        case eSnepClientState_OFF:
        {
        } break;
        case eSnepClientState_WAIT_READY:
        {
            g_SnepClientState = eSnepClientState_OFF;
            framework_NotifyMutex(g_SnepClientLock, 0);
        } break;
        case eSnepClientState_READY:
        {
            g_SnepClientState = eSnepClientState_OFF;
        } break;
        case eSnepClientState_EXIT:
        {
        } break;
    }
    
    framework_UnlockMutex(g_SnepClientLock);
}

void onMessageReceived(unsigned char *message, unsigned int length)
{
    unsigned int i = 0x00;
    PRINT_TXT("\n\t\tNDEF Message Received : \n");
    PrintNDEFContent(NULL, NULL, message, length);
}
void onSnepClientReady()
{
    framework_LockMutex(g_devLock);
    
    switch(g_DevState)
    {
        case eDevState_WAIT_DEPARTURE:
        {
            g_DevState = eDevState_PRESENT;
            g_Dev_Type = eDevType_P2P;
            framework_NotifyMutex(g_devLock, 0);
        } break;
        case eDevState_EXIT:
        {
            g_Dev_Type = eDevType_P2P;
        } break;
        case eDevState_NONE:
        {
            g_DevState = eDevState_PRESENT;
            g_Dev_Type = eDevType_P2P;
        } break;
        case eDevState_WAIT_ARRIVAL:
        {
            g_DevState = eDevState_PRESENT;
            g_Dev_Type = eDevType_P2P;
            framework_NotifyMutex(g_devLock, 0);
        } break;
        case eDevState_PRESENT:
        {
            g_Dev_Type = eDevType_P2P;
        } break;
        case eDevState_DEPARTED:
        {
            g_Dev_Type = eDevType_P2P;
            g_DevState = eDevState_PRESENT;
        } break;
    }
    framework_UnlockMutex(g_devLock);
    
    framework_LockMutex(g_SnepClientLock);
    
    switch(g_SnepClientState)
    {
        case eSnepClientState_WAIT_OFF:
        {
            g_SnepClientState = eSnepClientState_READY;
            framework_NotifyMutex(g_SnepClientLock, 0);
        } break;
        case eSnepClientState_OFF:
        {
            g_SnepClientState = eSnepClientState_READY;
        } break;
        case eSnepClientState_WAIT_READY:
        {
            g_SnepClientState = eSnepClientState_READY;
            framework_NotifyMutex(g_SnepClientLock, 0);
        } break;
        case eSnepClientState_READY:
        {
        } break;
        case eSnepClientState_EXIT:
        {
        } break;
    }
    
    framework_UnlockMutex(g_SnepClientLock);
}

void onSnepClientClosed()
{
    framework_LockMutex(g_devLock);
    
    switch(g_DevState)
    {
        case eDevState_WAIT_DEPARTURE:
        {
            g_DevState = eDevState_DEPARTED;
            g_Dev_Type = eDevType_NONE;
            framework_NotifyMutex(g_devLock, 0);
        } break;
        case eDevState_EXIT:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
        case eDevState_NONE:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
        case eDevState_WAIT_ARRIVAL:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
        case eDevState_PRESENT:
        {
            g_Dev_Type = eDevType_NONE;
            g_DevState = eDevState_DEPARTED;
        } break;
        case eDevState_DEPARTED:
        {
            g_Dev_Type = eDevType_NONE;
        } break;
    }
    framework_UnlockMutex(g_devLock);
    
    framework_LockMutex(g_SnepClientLock);
    
    switch(g_SnepClientState)
    {
        case eSnepClientState_WAIT_OFF:
        {
            g_SnepClientState = eSnepClientState_OFF;
            framework_NotifyMutex(g_SnepClientLock, 0);
        } break;
        case eSnepClientState_OFF:
        {
        } break;
        case eSnepClientState_WAIT_READY:
        {
            g_SnepClientState = eSnepClientState_OFF;
            framework_NotifyMutex(g_SnepClientLock, 0);
        } break;
        case eSnepClientState_READY:
        {
            g_SnepClientState = eSnepClientState_OFF;
        } break;
        case eSnepClientState_EXIT:
        {
        } break;
    }
    
    framework_UnlockMutex(g_SnepClientLock);
}
 
int InitMode(int tag, int p2p, int hce)
{
    int res = 0x00;
    
    g_TagCB.onTagArrival = onTagArrival;
    g_TagCB.onTagDeparture = onTagDeparture;
        
    g_SnepServerCB.onDeviceArrival = onDeviceArrival;
    g_SnepServerCB.onDeviceDeparture = onDeviceDeparture;
    g_SnepServerCB.onMessageReceived = onMessageReceived;
    
    g_SnepClientCB.onDeviceArrival = onSnepClientReady;
    g_SnepClientCB.onDeviceDeparture = onSnepClientClosed;
        
    g_HceCB.onDataReceived = onDataReceived;
    g_HceCB.onHostCardEmulationActivated = onHostCardEmulationActivated;
    g_HceCB.onHostCardEmulationDeactivated = onHostCardEmulationDeactivated;
    
    if(0x00 == res)
    {
        res = nfcManager_doInitialize();
        if(0x00 != res)
        {
            LOG("NfcService Init Failed\n");
        }
    }
    
    if(0x00 == res)
    {
        if(0x01 == tag)
        {
            nfcManager_registerTagCallback(&g_TagCB);
        }
        
        if(0x01 == p2p)
        {
            res = nfcSnep_registerClientCallback(&g_SnepClientCB);
            if(0x00 != res)
            {
                LOG("SNEP Client Register Callback Failed\n");
            }
        }
    }
    
    if(0x00 == res && 0x01 == hce)
    {
        nfcHce_registerHceCallback(&g_HceCB);
    }
    if(0x00 == res)
    {
        nfcManager_enableDiscovery(DEFAULT_NFA_TECH_MASK, 0x00, hce, 0);
        if(0x01 == p2p)
        {
            res = nfcSnep_startServer(&g_SnepServerCB);
            if(0x00 != res)
            {
                LOG("Start SNEP Server Failed\n");
            }
        }
    }
    
    return res;
}

int DeinitPollMode()
{
    int res = 0x00;
    
    nfcSnep_stopServer();
    
    nfcManager_disableDiscovery();
    
    nfcSnep_deregisterClientCallback();
    
    nfcManager_deregisterTagCallback();
    
    nfcHce_deregisterHceCallback();
    
    res = nfcManager_doDeinitialize();
    
    if(0x00 != res)
    {
        LOG("NFC Service Deinit Failed\n");
    }
    return res;
}

int SnepPush(unsigned char* msgToPush, unsigned int len)
{
    int res = 0x00;
    
    framework_LockMutex(g_devLock);
    framework_LockMutex(g_SnepClientLock);
    
    if(eSnepClientState_READY != g_SnepClientState && eSnepClientState_EXIT!= g_SnepClientState && eDevState_PRESENT == g_DevState)
    {
        framework_UnlockMutex(g_devLock);
        g_SnepClientState = eSnepClientState_WAIT_READY;
        framework_WaitMutex(g_SnepClientLock, 0);
    }
    else
    {
        framework_UnlockMutex(g_devLock);
    }
    
    if(eSnepClientState_READY == g_SnepClientState)
    {
        framework_UnlockMutex(g_SnepClientLock);
        res = nfcSnep_putMessage(msgToPush, len);
        
        if(0x00 != res)
        {
            LOG("\t\tPush Failed\n");
        }
        else
        {
            LOG("\t\tPush successful\n");
        }
    }
    else
    {
        framework_UnlockMutex(g_SnepClientLock);
    }
    
    return res;
}

int WriteTag(nfc_tag_info_t TagInfo, unsigned char* msgToPush, unsigned int len)
{
    int res = 0x00;
    
    res = nfcTag_writeNdef(TagInfo.handle, msgToPush, len);
    if(0x00 != res)
    {
        res = 0xFF;
    }
    else
    {
        res = 0x00;
    }
    return res;
}

void PrintfNDEFInfo(ndef_info_t pNDEFinfo)
{
    if(0x01 == pNDEFinfo.is_ndef)
    {
        PRINT_TXT("\t\tRecord Found :\n");
        PRINT_TXT("\t\t\t\tNDEF Content Max size :     '%d bytes'\n", pNDEFinfo.max_ndef_length);
        PRINT_TXT("\t\t\t\tNDEF Actual Content size :     '%d bytes'\n", pNDEFinfo.current_ndef_length);
        if(0x01 == pNDEFinfo.is_writable)
        {
           PRINT_TXT("\t\t\t\tReadOnly :                      'FALSE'\n");
           PRINT_JSON(", \"read_only\" : false");
        }
        else
        {
           PRINT_TXT("\t\t\t\tReadOnly :                         'TRUE'\n");
           PRINT_JSON(", \"read_only\" : true");
        }
    }
    else    
    {
       PRINT_TXT("\t\tNo Record found\n");
    }
}

void open_uri (const char* uri)
{
    char *temp = malloc(strlen("xdg-open ") + strlen(uri) + 1);
    if (temp != NULL)
    {
        strcpy(temp, "xdg-open ");
        strcat(temp, uri);
        strcat(temp, "&");
        LOG("\t\t- Opening URI in web browser ...\n");
        system(temp);
        free(temp);
    }
}

void PrintNDEFContent(nfc_tag_info_t* TagInfo, ndef_info_t* NDEFinfo, unsigned char* ndefRaw, unsigned int ndefRawLen)
{
    unsigned char* NDEFContent = NULL;
    nfc_friendly_type_t lNDEFType = NDEF_FRIENDLY_TYPE_OTHER;
    unsigned int res = 0x00;
    unsigned int i = 0x00;
    unsigned int langCode_len;
    char* LanguageCode = NULL;
    char* TextContent = NULL;
    char* URLContent = NULL;
    nfc_handover_select_t HandoverSelectContent;
    nfc_handover_request_t HandoverRequestContent;
    if(NULL != NDEFinfo)
    {
        ndefRawLen = NDEFinfo->current_ndef_length;
        NDEFContent = malloc(ndefRawLen * sizeof(unsigned char));
        res = nfcTag_readNdef(TagInfo->handle, NDEFContent, ndefRawLen, &lNDEFType);
    }
    else if (NULL != ndefRaw && 0x00 != ndefRawLen)
    {
        NDEFContent = malloc(ndefRawLen * sizeof(unsigned char));
        memcpy(NDEFContent, ndefRaw, ndefRawLen);
        res = ndefRawLen;
        if((NDEFContent[0] & 0x7) == NDEF_TNF_WELLKNOWN && 0x55 == NDEFContent[3])
        {
            lNDEFType = NDEF_FRIENDLY_TYPE_URL;
        }
        if((NDEFContent[0] & 0x7) == NDEF_TNF_WELLKNOWN && 0x54 == NDEFContent[3])
        {
            lNDEFType = NDEF_FRIENDLY_TYPE_TEXT;
        }
    }
    else
    {
        LOG("\t\t\t\tError : Invalid Parameters\n");
    }

    if(res != ndefRawLen)
    {
        LOG("\t\t\t\tRead NDEF Content Failed\n");
    }
    else
    {
        switch(lNDEFType)
        {
            case NDEF_FRIENDLY_TYPE_TEXT:
            {
                TextContent = malloc(res * sizeof(char));
                langCode_len = ndef_readLanguageCode(NDEFContent, res, TextContent, res);
                if(0x00 <= langCode_len)
                {
                    LanguageCode = malloc((langCode_len + 1) * sizeof(char));
                    memcpy(LanguageCode, TextContent, langCode_len);
                    LanguageCode[langCode_len] = '\0'; /* Add extra character for proper display */
                    res = ndef_readText(NDEFContent, res, TextContent, res);
                    if(0x00 <= res)
                    {
                        TextContent[res] = '\0'; /* Add extra character for proper display */
                        PRINT_TXT("\t\t\t\tType :                 'Text'\n");
                        PRINT_TXT("\t\t\t\tLang :                 '%s'\n", LanguageCode);
                        PRINT_TXT("\t\t\t\tText :                 '%s'\n\n", TextContent);
                    }
                    else
                    {
                        LOG("\t\t\t\tRead NDEF Text Error\n");
                    }
                }
                if(NULL != TextContent)
                {
                    free(TextContent);
                    TextContent = NULL;
                }
            } break;
            case NDEF_FRIENDLY_TYPE_URL:
            {
                /*NOTE : + 27 = Max prefix lenght*/
                URLContent = malloc(res * sizeof(unsigned char) + 27 );
                memset(URLContent, 0x00, res * sizeof(unsigned char) + 27);
                res = ndef_readUrl(NDEFContent, res, URLContent, res + 27);
                if(0x00 <= res)
                {
                   PRINT_TXT("\t\t\t\tType :                 'URI'\n");
                   PRINT_TXT("\t\t\t\t URI :                 '%s'\n\n", URLContent);
                    /*NOTE: open url in browser*/
                    /*open_uri(URLContent);*/
                }
                else
                {
                    LOG("                Read NDEF URL Error\n");
                }
                if(NULL != URLContent)
                {
                    free(URLContent);
                    URLContent = NULL;
                }
            } break;
            case NDEF_FRIENDLY_TYPE_HS:
            {
                res = ndef_readHandoverSelectInfo(NDEFContent, res, &HandoverSelectContent);
                if(0x00 <= res)
                {
                    PRINT_TXT("\n\t\tHandover Select : \n");
                    
                    PRINT_TXT("\t\tBluetooth : \n\t\t\t\tPower state : ");
                    switch(HandoverSelectContent.bluetooth.power_state)
                    {
                        case HANDOVER_CPS_INACTIVE:
                        {
                            PRINT_TXT(" 'Inactive'\n");
                        } break;
                        case HANDOVER_CPS_ACTIVE:
                        {
                            PRINT_TXT(" 'Active'\n");
                        } break;
                        case HANDOVER_CPS_ACTIVATING:
                        {
                            PRINT_TXT(" 'Activating'\n");
                        } break;
                        case HANDOVER_CPS_UNKNOWN:
                        {
                            PRINT_TXT(" 'Unknown'\n");
                        } break;
                        default:
                        {
                            PRINT_TXT(" 'Unknown'\n");
                        } break;
                    }
                    if(HANDOVER_TYPE_BT == HandoverSelectContent.bluetooth.type)
                    {
                        PRINT_TXT("\t\t\t\tType :         'BT'\n");
                    }
                    else if(HANDOVER_TYPE_BLE == HandoverSelectContent.bluetooth.type)
                    {
                        PRINT_TXT("\t\t\t\tType :         'BLE'\n");
                    }
                    else
                    {
                        PRINT_TXT("\t\t\t\tType :            'Unknown'\n");
                    }
                    PRINT_TXT("\t\t\t\tAddress :      '");
                    for(i = 0x00; i < 6; i++)
                    {
                        PRINT_TXT("%02X ", HandoverSelectContent.bluetooth.address[i]);
                    }
                    PRINT_TXT("'\n\t\t\t\tDevice Name :  '");
                    for(i = 0x00; i < HandoverSelectContent.bluetooth.device_name_length; i++)    
                    {
                        PRINT_TXT("%c ", HandoverSelectContent.bluetooth.device_name[i]);
                    }
                    PRINT_TXT("'\n\t\t\t\tNDEF Record :     \n\t\t\t\t");
                    for(i = 0x01; i < HandoverSelectContent.bluetooth.ndef_length+1; i++)
                    {
                        PRINT_TXT("%02X ", HandoverSelectContent.bluetooth.ndef[i]);
                        if(i%8 == 0)
                        {
                            PRINT_TXT("\n\t\t\t\t");
                        }
                    }
                    PRINT_TXT("\n\t\tWIFI : \n\t\t\t\tPower state : ");
                    switch(HandoverSelectContent.wifi.power_state)
                    {
                        case HANDOVER_CPS_INACTIVE:
                        {
                            PRINT_TXT(" 'Inactive'\n");
                        } break;
                        case HANDOVER_CPS_ACTIVE:
                        {
                            PRINT_TXT(" 'Active'\n");
                        } break;
                        case HANDOVER_CPS_ACTIVATING:
                        {
                            PRINT_TXT(" 'Activating'\n");
                        } break;
                        case HANDOVER_CPS_UNKNOWN:
                        {
                            PRINT_TXT(" 'Unknown'\n");
                        } break;
                        default:
                        {
                            PRINT_TXT(" 'Unknown'\n");
                        } break;
                    }
                    
                    PRINT_TXT("\t\t\t\tSSID :         '");
                    for(i = 0x01; i < HandoverSelectContent.wifi.ssid_length+1; i++)
                    {
                        PRINT_TXT("%02X ", HandoverSelectContent.wifi.ssid[i]);
                        if(i%30 == 0)
                        {
                            PRINT_TXT("\n");
                        }
                    }
                    PRINT_TXT("'\n\t\t\t\tKey :          '");
                    for(i = 0x01; i < HandoverSelectContent.wifi.key_length+1; i++)
                    {
                        PRINT_TXT("%02X ", HandoverSelectContent.wifi.key[i]);
                        if(i%30 == 0)
                        {
                            PRINT_TXT("\n");
                        }
                    }                
                    PRINT_TXT("'\n\t\t\t\tNDEF Record : \n");
                    for(i = 0x01; i < HandoverSelectContent.wifi.ndef_length+1; i++)
                    {
                        PRINT_TXT("%02X ", HandoverSelectContent.wifi.ndef[i]);
                        if(i%30 == 0)
                        {
                            PRINT_TXT("\n");
                        }
                    }
                    PRINT_TXT("\n");
                }
                else
                {
                    LOG("\n\t\tRead NDEF Handover Select Failed\n");
                }
                
            } break;
            case NDEF_FRIENDLY_TYPE_HR:
            {
                res = ndef_readHandoverRequestInfo(NDEFContent, res, &HandoverRequestContent);
                if(0x00 <= res)
                {
                    PRINT_TXT("\n\t\tHandover Request : \n");
                    PRINT_TXT("\t\tBluetooth : \n\t\t\t\tPower state : ");
                    switch(HandoverRequestContent.bluetooth.power_state)
                    {
                        case HANDOVER_CPS_INACTIVE:
                        {
                            PRINT_TXT(" 'Inactive'\n");
                        } break;
                        case HANDOVER_CPS_ACTIVE:
                        {
                            PRINT_TXT(" 'Active'\n");
                        } break;
                        case HANDOVER_CPS_ACTIVATING:
                        {
                            PRINT_TXT(" 'Activating'\n");
                        } break;
                        case HANDOVER_CPS_UNKNOWN:
                        {
                            PRINT_TXT(" 'Unknown'\n");
                        } break;
                        default:
                        {
                            PRINT_TXT(" 'Unknown'\n");
                        } break;
                    }
                    if(HANDOVER_TYPE_BT == HandoverRequestContent.bluetooth.type)
                    {
                        PRINT_TXT("\t\t\t\tType :         'BT'\n");
                    }
                    else if(HANDOVER_TYPE_BLE == HandoverRequestContent.bluetooth.type)
                    {
                        PRINT_TXT("\t\t\t\tType :         'BLE'\n");
                    }
                    else
                    {
                        PRINT_TXT("\t\t\t\tType :            'Unknown'\n");
                    }
                    PRINT_TXT("\t\t\t\tAddress :      '");
                    for(i = 0x00; i < 6; i++)
                    {
                        PRINT_TXT("%02X ", HandoverRequestContent.bluetooth.address[i]);
                    }
                    PRINT_TXT("'\n\t\t\t\tDevice Name :  '");
                    for(i = 0x00; i < HandoverRequestContent.bluetooth.device_name_length; i++)    
                    {
                        PRINT_TXT("%c ", HandoverRequestContent.bluetooth.device_name[i]);
                    }
                    PRINT_TXT("'\n\t\t\t\tNDEF Record :     \n\t\t\t\t");
                    for(i = 0x01; i < HandoverRequestContent.bluetooth.ndef_length+1; i++)
                    {
                        PRINT_TXT("%02X ", HandoverRequestContent.bluetooth.ndef[i]);
                        if(i%8 == 0)
                        {
                            PRINT_TXT("\n\t\t\t\t");
                        }
                    }
                    PRINT_TXT("\n\t\t\t\tWIFI :         'Has WIFI Request : %X '", HandoverRequestContent.wifi.has_wifi);
                    PRINT_TXT("\n\t\t\t\tNDEF Record :     \n\t\t\t\t");
                    for(i = 0x01; i < HandoverRequestContent.wifi.ndef_length+1; i++)
                    {
                        PRINT_TXT("%02X ", HandoverRequestContent.wifi.ndef[i]);
                        if(i%8 == 0)
                        {
                            PRINT_TXT("\n\t\t\t\t");
                        }
                    }
                    PRINT_TXT("\n");
                }
                else
                {
                    LOG("\n\t\tRead NDEF Handover Request Failed\n");
                }
            } break;
            case NDEF_FRIENDLY_TYPE_OTHER:
            {
                switch(NDEFContent[0] & 0x7)
                {
                    case NDEF_TNF_EMPTY:
                    {
                        PRINT_TXT("\n\t\tTNF Empty\n");
                    } break;
                    case NDEF_TNF_WELLKNOWN:
                    {
                        PRINT_TXT("\n\t\tTNF Well Known\n");
                    } break;
                    case NDEF_TNF_MEDIA:
                    {
                        PRINT_TXT("\n\t\tTNF Media\n\n");
                        PRINT_TXT("\t\t\tType : ");
                        for(i = 0x00; i < NDEFContent[1]; i++)
                        {
                            PRINT_TXT("%c", NDEFContent[3 + i]);
                        }
                        PRINT_TXT("\n\t\t\tData : ");
                        for(i = 0x00; i < NDEFContent[2]; i++)
                         {
                            PRINT_TXT("%c", NDEFContent[3 + NDEFContent[1] + i]);
                            if('\n' == NDEFContent[3 + NDEFContent[1] + i])
                            {
                                PRINT_TXT("\t\t\t");
                            }
                        }
                        PRINT_TXT("\n");
                        
                    } break;
                    case NDEF_TNF_URI:
                    {
                        PRINT_TXT("\n\t\tTNF URI\n");
                    } break;
                    case NDEF_TNF_EXT:
                    {
                        PRINT_TXT("\n\t\tTNF External\n\n");
                        PRINT_TXT("\t\t\tType : ");
                        for(i = 0x00; i < NDEFContent[1]; i++)
                        {
                            PRINT_TXT("%c", NDEFContent[3 + i]);
                        }
                        PRINT_TXT("\n\t\t\tData : ");
                        for(i = 0x00; i < NDEFContent[2]; i++)
                         {
                            PRINT_TXT("%c", NDEFContent[3 + NDEFContent[1] + i]);
                            if('\n' == NDEFContent[3 + NDEFContent[1] + i])
                            {
                                PRINT_TXT("\t\t\t");
                            }
                        }
                        PRINT_TXT("\n");
                    } break;
                    case NDEF_TNF_UNKNOWN:
                    {
                        PRINT_TXT("\n\t\tTNF Unknown\n");
                    } break;
                    case NDEF_TNF_UNCHANGED:
                    {
                        PRINT_TXT("\n\t\tTNF Unchanged\n");
                    } break;
                    default:
                    {
                        PRINT_TXT("\n\t\tTNF Other\n");
                    } break;
                }
            } break;
            default:
            {
            } break;
        }

        PRINT_JSON(", \"ndef_max\" : %d", NDEFinfo->max_ndef_length);

        PRINT_TXT("\n\t\t%d bytes of NDEF data received :\n\t\t", ndefRawLen);
        PRINT_JSON(", \"ndef_sz\" : %d, \"ndef\" : \"", ndefRawLen);

        for(i = 0x00; i < ndefRawLen; i++)
        {
            PRINT_TXT("%02X", NDEFContent[i]);
            PRINT_JSON("%02X", NDEFContent[i]);

            if (i < (ndefRawLen - 1))
            {
                PRINT_TXT(" ");
                PRINT_JSON(" ");
            }

            if(i%30 == 0 && 0x00 != i)
            {
                PRINT_TXT("\n\t\t");
            }
        }
        PRINT_TXT("\n\n");
        PRINT_JSON("\"");
    }

    if(NULL != NDEFContent)
    {
        free(NDEFContent);
        NDEFContent = NULL;
    }
}

/* mode=1 => poll, mode=2 => push, mode=3 => write, mode=4 => HCE */
int WaitDeviceArrival(int mode, unsigned char* msgToSend, unsigned int len)
{
    int res = 0x00;
    unsigned int i = 0x00;
    int block = 0x01;
    unsigned char key[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ndef_info_t NDEFinfo;
    eDevType DevTypeBck = eDevType_NONE;
    unsigned char MifareAuthCmd[] = {0x60U, 0x00 /*block*/, 0x02, 0x02, 0x02, 0x02, 0x00 /*key*/, 0x00 /*key*/, 0x00 /*key*/, 0x00 /*key*/ , 0x00 /*key*/, 0x00 /*key*/};
    unsigned char MifareAuthResp[255];
    unsigned char MifareReadCmd[] = {0x30U,  /*block*/ 0x00};
    unsigned char MifareWriteCmd[] = {0xA2U,  /*block*/ 0x04, 0xFF, 0xFF, 0xFF, 0xFF};
    unsigned char MifareResp[255];
    
    unsigned char HCEReponse[255];
    short unsigned int HCEResponseLen = 0x00;
    int tag_count=0;
    int num_tags = 0;
    
    nfc_tag_info_t TagInfo;
    
    MifareAuthCmd[1] = block;
    memcpy(&MifareAuthCmd[6], key, 6);
    MifareReadCmd[1] = block;
    
    do
    {
        framework_LockMutex(g_devLock);
        if(eDevState_EXIT == g_DevState)
        {
            framework_UnlockMutex(g_devLock);
            break;
        }
        
        else if(eDevState_PRESENT != g_DevState)
        {
               if(tag_count == 0) LOG("\nWaiting for a Tag/Device...\n\n");
            g_DevState = eDevState_WAIT_ARRIVAL;
            framework_WaitMutex(g_devLock, 0);
        }
        
        if(eDevState_EXIT == g_DevState)
        {
            framework_UnlockMutex(g_devLock);
            break;
        }
        
        if(eDevState_PRESENT == g_DevState)
        {
            PRINT_JSON("{ \"counter\" : %d", g_msgCounter++);

            DevTypeBck = g_Dev_Type;
            if(eDevType_TAG == g_Dev_Type)
            {
                memcpy(&TagInfo, &g_TagInfo, sizeof(nfc_tag_info_t));
                framework_UnlockMutex(g_devLock);
                PRINT_TXT("        Type : ");
                switch (TagInfo.technology)
                {
                    case TARGET_TYPE_UNKNOWN:
                    {
                        PRINT_TXT("        'Type Unknown'\n");
                    } break;
                    case TARGET_TYPE_ISO14443_3A:
                    {
                        PRINT_TXT("        'Type A'\n");
                    } break;
                    case TARGET_TYPE_ISO14443_3B:
                    {
                        PRINT_TXT("        'Type 4B'\n");
                    } break;
                    case TARGET_TYPE_ISO14443_4:
                    {
                        PRINT_TXT("        'Type 4A'\n");
                    } break;
                    case TARGET_TYPE_FELICA:
                    {
                        PRINT_TXT("        'Type F'\n");
                    } break;
                    case TARGET_TYPE_ISO15693:
                    {
                        PRINT_TXT("        'Type V'\n");
                    } break;
                    case TARGET_TYPE_NDEF:
                    {
                        PRINT_TXT("        'Type NDEF'\n");
                    } break;
                    case TARGET_TYPE_NDEF_FORMATABLE:
                    {
                        PRINT_TXT("        'Type Formatable'\n");
                    } break;
                    case TARGET_TYPE_MIFARE_CLASSIC:
                    {
                        PRINT_TXT("        'Type A - Mifare Classic'\n");
                    } break;
                    case TARGET_TYPE_MIFARE_UL:
                    {
                        PRINT_TXT("        'Type A - Mifare Ul'\n");
                    } break;
                    case TARGET_TYPE_KOVIO_BARCODE:
                    {
                        PRINT_TXT("        'Type A - Kovio Barcode'\n");
                    } break;
                    case TARGET_TYPE_ISO14443_3A_3B:
                    {
                        PRINT_TXT("        'Type A/B'\n");
                    } break;
                    default:
                    {
                        PRINT_TXT("        'Type %d (Unknown or not supported)'\n", TagInfo.technology);
                    } break;
                }
                /*32 is max UID len (Kovio tags)*/
                if((0x00 != TagInfo.uid_length) && (32 >= TagInfo.uid_length))
                {
                    if(4 == TagInfo.uid_length || 7 == TagInfo.uid_length || 10 == TagInfo.uid_length)
                    {
                        PRINT_TXT("        NFCID1 :    \t'");
                    }
                    else if(8 == TagInfo.uid_length)
                    {
                        PRINT_TXT("        NFCID2 :    \t'");
                    }
                    else
                    {
                        PRINT_TXT("        UID :       \t'");
                    }
                    PRINT_JSON(", \"id_sz\" : %d, \"id\" : \"", TagInfo.uid_length);
                    
                    for(i = 0x00; i < TagInfo.uid_length; i++)
                    {
                        PRINT_TXT("%02X", (unsigned char) TagInfo.uid[i]);
                        PRINT_JSON("%02X", (unsigned char) TagInfo.uid[i]);

                        if (i < (TagInfo.uid_length - 1))
                        {
                            PRINT_TXT(" ");
                            PRINT_JSON(" ");
                        }
                    }
                    PRINT_TXT("'\n");
                    PRINT_JSON("\"");
                }
                res = nfcTag_isNdef(TagInfo.handle, &NDEFinfo);
                if(0x01 == res)
                {
                    PrintfNDEFInfo(NDEFinfo);
                    PrintNDEFContent(&TagInfo, &NDEFinfo, NULL, 0x00);
                }
                else
                {
                    PRINT_TXT("\t\tNDEF Content : NO, mode=%d, tech=%d\n", mode, TagInfo.technology);
                    
                    if(0x03 == mode)
                    {
                         PRINT_TXT("\n\tFormating tag to NDEF prior to write ...\n");
                        if(nfcTag_isFormatable(TagInfo.handle))
                        {
                            if(nfcTag_formatTag(TagInfo.handle) == 0x00)
                            {
                                  LOG("\tTag formating succeed\n");
                            }
                            else
                            {
                                  LOG("\tTag formating failed\n");
                            }
                        }
                        else
                        {
                              LOG("\tTag is not formatable\n");
                        }
                    }
                    else if(TARGET_TYPE_MIFARE_CLASSIC == TagInfo.technology)
                    {
                        memset(MifareAuthResp, 0x00, 255);
                        memset(MifareResp, 0x00, 255);
                        res = nfcTag_transceive(TagInfo.handle, MifareAuthCmd, 12, MifareAuthResp, 255, 500);
                        if(0x00 == res)
                        {
                            LOG("\n\t\tRAW Tag transceive failed\n");
                        }
                        else
                        {
                            PRINT_TXT("\n\t\tMifare Authenticate command sent\n\t\tResponse : \n\t\t");
                            for(i = 0x00; i < (unsigned int) res; i++)
                            {
                                PRINT_TXT("%02X ", MifareAuthResp[i]);
                            }
                            PRINT_TXT("\n");
                            
                            res = nfcTag_transceive(TagInfo.handle, MifareReadCmd, 2, MifareResp, 255, 500);
                            if(0x00 == res)
                            {
                                LOG("\n\t\tRAW Tag transceive failed\n");
                            }
                            else
                            {
                                PRINT_TXT("\n\t\tMifare Read command sent\n\t\tResponse : \n\t\t");
                                for(i = 0x00; i < (unsigned int)res; i++)
                                {
                                    PRINT_TXT("%02X ", MifareResp[i]);
                                }
                                PRINT_TXT("\n\n");
                            }
                        }
                    }
                    else if(TARGET_TYPE_MIFARE_UL == TagInfo.technology)
                    {
                        PRINT_TXT("\n\tMIFARE UL card\n");
                        PRINT_TXT("\t\tMifare Read command: ");
                        for(i = 0x00; i < (unsigned int) sizeof(MifareReadCmd) ; i++)
                        {
                            PRINT_TXT("%02X ", MifareReadCmd[i]);
                        }
                        PRINT_TXT("\n");
                        res = nfcTag_transceive(TagInfo.handle, MifareReadCmd, sizeof(MifareReadCmd), MifareResp, 16, 500);
                        if(0x00 == res)
                        {
                            LOG("\n\t\tRAW Tag transceive failed\n");
                        }
                        else
                        {
                            LOG("\n\t\tMifare Read command sent\n\t\tResponse : \n\t\t");
                            for(i = 0x00; i < (unsigned int)res; i++)
                            {
                                LOG("%02X ", MifareResp[i]);
                            }
                            LOG("\n\n");
                        }
                    }
                    else
                    {
                        PRINT_TXT("\n\tNot a MIFARE card\n");
                    }

                }

                PRINT_JSON(" }\n");

                if (g_OutputFile) {
                    fprintf(g_OutputFile, "%s", g_jsonBuf);
                    ftruncate(fileno(g_OutputFile), strlen(g_jsonBuf));
                    fseek(g_OutputFile, 0, SEEK_SET);
                    g_jsonBuf[0] = '\0';
                }

                // standard output is appended continuously, whereas if output file has
                // been specified, we rewind it after each message and write new content
                fflush((g_OutputFile ? g_OutputFile : stdout));

                if (g_doBeep)
                    do_beep(2000);

                if(0x03 == mode)
                {
                    res = WriteTag(TagInfo, msgToSend, len);
                    if(0x00 == res)
                    {
                        LOG("\tWrite Tag OK\n\tRead back data\n");
                        res = nfcTag_isNdef(TagInfo.handle, &NDEFinfo);
                        if(0x01 == res)
                        {
                            PrintfNDEFInfo(NDEFinfo);
                            PrintNDEFContent(&TagInfo, &NDEFinfo, NULL, 0x00);
                        }
                    }
                    else
                    {
                        LOG("\tWrite Tag Failed\n");
                    }
                }
                num_tags = nfcManager_getNumTags();
                if(num_tags > 1)
                {
                    tag_count++;
                    if (tag_count < num_tags)
                    {
                        LOG("\tMultiple tags found, selecting next tag...\n");
                        nfcManager_selectNextTag();
                    }
                    else
                    {
                        tag_count = 0;
                    }
                }
                 framework_LockMutex(g_devLock);
            }
            else if(eDevType_P2P == g_Dev_Type)/*P2P Detected*/
            {
                framework_UnlockMutex(g_devLock);
                PRINT_TXT("\tDevice Found\n");
                
                if(2 == mode)
                {
                    SnepPush(msgToSend, len);
                }
                
                framework_LockMutex(g_SnepClientLock);
    
                if(eSnepClientState_READY == g_SnepClientState)
                {
                    g_SnepClientState = eSnepClientState_WAIT_OFF;
                    framework_WaitMutex(g_SnepClientLock, 0);
                }
                
                framework_UnlockMutex(g_SnepClientLock);
                framework_LockMutex(g_devLock);
        
            }
            else if(eDevType_READER == g_Dev_Type)
            {                
                framework_LockMutex(g_HCELock);
                do
                {
                    framework_UnlockMutex(g_devLock);
                
                    if(eHCEState_NONE == g_HCEState)
                    {
                        g_HCEState = eHCEState_WAIT_DATA;
                        framework_WaitMutex(g_HCELock, 0x00);
                    }
                    
                    if(eHCEState_DATA_RECEIVED == g_HCEState)
                    {
                        g_HCEState = eHCEState_NONE;
                        
                        if(HCE_data != NULL)
                        {
                            PRINT_TXT("\t\tReceived data from remote device : \n\t\t");
                            
                            for(i = 0x00; i < HCE_dataLenght; i++)
                            {
                                PRINT_TXT("%02X ", HCE_data[i]);
                            }
                            
                            /*Call HCE response builder*/
                            T4T_NDEF_EMU_Next(HCE_data, HCE_dataLenght, HCEReponse, &HCEResponseLen);
                            free(HCE_data);
                            HCE_dataLenght = 0x00;
                            HCE_data = NULL;
                        }
                        framework_UnlockMutex(g_HCELock);
                        res = nfcHce_sendCommand(HCEReponse, HCEResponseLen);
                        framework_LockMutex(g_HCELock);
                        if(0x00 == res)
                        {
                            PRINT_TXT("\n\n\t\tResponse sent : \n\t\t");
                            for(i = 0x00; i < HCEResponseLen; i++)
                            {
                                PRINT_TXT("%02X ", HCEReponse[i]);
                            }
                            PRINT_TXT("\n\n");
                        }
                        else
                        {
                            LOG("\n\n\t\tFailed to send response\n\n");
                        }
                    }
                    framework_LockMutex(g_devLock);
                }while(eDevState_PRESENT == g_DevState);
                framework_UnlockMutex(g_HCELock);
            }
            else
            {
                framework_UnlockMutex(g_devLock);
                break;
            }
            
            if(eDevState_PRESENT == g_DevState)
            {
                g_DevState = eDevState_WAIT_DEPARTURE;
                framework_WaitMutex(g_devLock, 0);
                if(eDevType_P2P == DevTypeBck)
                {
                    LOG("\tDevice Lost\n\n");
                }
                DevTypeBck = eDevType_NONE;
            }
            else if(eDevType_P2P == DevTypeBck)
            {
                LOG("\tDevice Lost\n\n");
            }
        }
        
        framework_UnlockMutex(g_devLock);
    }while(0x01);
    
    return res;
}

void strtolower(char * string) 
{
    unsigned int i = 0x00;
    
    for(i = 0; i < strlen(string); i++) 
    {
        string[i] = tolower(string[i]);
    }
}

char* strRemovceChar(const char* str, char car)
{
    unsigned int i = 0x00;
    unsigned int index = 0x00;
    char * dest = (char*)malloc((strlen(str) + 1) * sizeof(char));
    
    for(i = 0x00; i < strlen(str); i++)
    {
        if(str[i] != car)
        {
            dest[index++] = str[i];
        }
    }
    dest[index] = '\0';
    return dest;
}

int convertParamtoBuffer(char* param, unsigned char** outBuffer, unsigned int* outBufferLen)
{
    int res = 0x00;
    unsigned int i = 0x00;
    int index = 0x00;
    char atoiBuf[3];
    atoiBuf[2] = '\0';
    
    if(NULL == param || NULL == outBuffer || NULL == outBufferLen)    
    {
        LOG("Parameter Error\n");
        res = 0xFF;
    }
    
    if(0x00 == res)
    {
        param = strRemovceChar(param, ' ');
    }
    
    if(0x00 == res)
    {
        if(0x00 == strlen(param) % 2)
        {
            *outBufferLen = strlen(param) / 2;
            
            *outBuffer = (unsigned char*) malloc((*outBufferLen) * sizeof(unsigned char));
            if(NULL != *outBuffer)
            {
                for(i = 0x00; i < ((*outBufferLen) * 2); i = i + 2)
                {
                    atoiBuf[0] = param[i];
                    atoiBuf[1] = param[i + 1];
                    (*outBuffer)[index++] = strtol(atoiBuf, NULL, 16);
                }
            }
            else
            {
                LOG("Memory Allocation Failed\n");
                res = 0xFF;
            }
        }
        else
        {
            LOG("Invalid NDEF Message Param\n");
        }
        free(param);
    }
        
    return res;
}

int BuildNDEFMessage(int arg_len, char** arg, unsigned char** outNDEFBuffer, unsigned int* outNDEFBufferLen)
{
    int res = 0x00;
    nfc_handover_cps_t cps = HANDOVER_CPS_UNKNOWN;
    unsigned char* ndef_msg = NULL;
    unsigned int ndef_msg_len = 0x00;
    char *type = NULL;
    char *uri = NULL;
    char *text = NULL;
    char* lang = NULL;
    char* mime_type = NULL;
    char* mime_data = NULL;
    char* carrier_state = NULL;
    char* carrier_name = NULL;
    char* carrier_data = NULL;
    
    if(0xFF == LookForTag(arg, arg_len, "-t", &type, 0x00) && 0xFF == LookForTag(arg, arg_len, "--type", &type, 0x01))
    {
        res = 0xFF;
        LOG("Record type missing (-t)\n");
        help(0x02);
    }
    if(0x00 == res)
    {
        strtolower(type);
        if(0x00 == strcmp(type, "uri"))
        {
            if(0x00 == LookForTag(arg, arg_len, "-u", &uri, 0x00) || 0x00 == LookForTag(arg, arg_len, "--uri", &uri, 0x01))
            {
                *outNDEFBufferLen = strlen(uri) + 30; /*TODO : replace 30 by URI NDEF message header*/
                *outNDEFBuffer = (unsigned char*) malloc(*outNDEFBufferLen * sizeof(unsigned char));
                res = ndef_createUri(uri, *outNDEFBuffer, *outNDEFBufferLen);
                if(0x00 >= res)
                {
                    LOG("Failed to build URI NDEF Message\n");
                }
                else
                {
                    *outNDEFBufferLen = res;
                    res = 0x00;
                }
            }
            else
            {
                LOG("URI Missing (-u)\n");
                help(0x02);
                res = 0xFF;
            }
        }
        else if(0x00 == strcmp(type, "text"))
        {
            if(0xFF == LookForTag(arg, arg_len, "-r", &text, 0x00) && 0xFF == LookForTag(arg, arg_len, "--rep", &text, 0x01))
            {
                LOG("Representation missing (-r)\n");
                res = 0xFF;
            }
            
            if(0xFF == LookForTag(arg, arg_len, "-l", &lang, 0x00) && 0xFF == LookForTag(arg, arg_len, "--lang", &lang, 0x01))
            {
                LOG("Language missing (-l)\n");
                res = 0xFF;
            }
            if(0x00 == res)
            {
                *outNDEFBufferLen = strlen(text) + strlen(lang) + 30; /*TODO : replace 30 by TEXT NDEF message header*/
                *outNDEFBuffer = (unsigned char*) malloc(*outNDEFBufferLen * sizeof(unsigned char));
                res = ndef_createText(lang, text, *outNDEFBuffer, *outNDEFBufferLen);
                if(0x00 >= res)
                {
                    LOG("Failed to build TEXT NDEF Message\n");
                }
                else
                {
                    *outNDEFBufferLen = res;
                    res = 0x00;
                }    
            }
            else
            {
                help(0x02);
            }
        }
        else if(0x00 == strcmp(type, "mime"))
        {
            if(0xFF == LookForTag(arg, arg_len, "-m", &mime_type, 0x00) && 0xFF == LookForTag(arg, arg_len, "--mime", &mime_type, 0x01))
            {
                LOG("Mime-type missing (-m)\n");
                res = 0xFF;
            }
            if(0xFF == LookForTag(arg, arg_len, "-d", &mime_data, 0x00) && 0xFF == LookForTag(arg, arg_len, "--data", &mime_data, 0x01))
            {
                LOG("NDEF Data missing (-d)\n");
                res = 0xFF;
            }
            if(0x00 == res)
            {
                *outNDEFBufferLen = strlen(mime_data) +  strlen(mime_type) + 30; /*TODO : replace 30 by MIME NDEF message header*/
                *outNDEFBuffer = (unsigned char*) malloc(*outNDEFBufferLen * sizeof(unsigned char));
                
                res = convertParamtoBuffer(mime_data, &ndef_msg, &ndef_msg_len);
                
                if(0x00 == res)
                {
                    res = ndef_createMime(mime_type, ndef_msg, ndef_msg_len, *outNDEFBuffer, *outNDEFBufferLen);
                    if(0x00 >= res)
                    {
                        LOG("Failed to build MIME NDEF Message\n");
                    }
                    else
                    {
                        *outNDEFBufferLen = res;
                        res = 0x00;
                    }
                }
            }
            else
            {
                help(0x02);
            }
        }
        else if(0x00 == strcmp(type, "hs"))
        {
            
            if(0xFF == LookForTag(arg, arg_len, "-cs", &carrier_state, 0x00) && 0xFF == LookForTag(arg, arg_len, "--carrierState", &carrier_state, 0x01))
            {
                LOG("Carrier Power State missing (-cs)\n");
                res = 0xFF;
            }
            if(0xFF == LookForTag(arg, arg_len, "-cn", &carrier_name, 0x00) && 0xFF == LookForTag(arg, arg_len, "--carrierName", &carrier_name, 0x01))
            {
                LOG("Carrier Reference Name missing (-cn)\n");
                res = 0xFF;
            }
            
            if(0xFF == LookForTag(arg, arg_len, "-d", &carrier_data, 0x00) && 0xFF == LookForTag(arg, arg_len, "--data", &carrier_data, 0x01))
            {
                LOG("NDEF Data missing (-d)\n");
                res = 0xFF;
            }
            
            if(0x00 == res)
            {
                *outNDEFBufferLen = strlen(carrier_name) + strlen(carrier_data) + 30;  /*TODO : replace 30 by HS NDEF message header*/
                *outNDEFBuffer = (unsigned char*) malloc(*outNDEFBufferLen * sizeof(unsigned char));
                
                strtolower(carrier_state);
                
                if(0x00 == strcmp(carrier_state, "inactive"))
                {
                    cps = HANDOVER_CPS_INACTIVE;
                }
                else if(0x00 == strcmp(carrier_state, "active"))
                {
                    cps = HANDOVER_CPS_ACTIVE;
                }
                else if(0x00 == strcmp(carrier_state, "activating"))
                {
                    cps = HANDOVER_CPS_ACTIVATING;
                }
                else
                {
                    LOG("Error : unknown carrier power state %s\n", carrier_state);
                    res = 0xFF;
                }
                
                if(0x00 == res)
                {
                    res = convertParamtoBuffer(carrier_data, &ndef_msg, &ndef_msg_len);
                }
                
                if(0x00 == res)
                {
                    res = ndef_createHandoverSelect(cps, carrier_name, ndef_msg, ndef_msg_len, *outNDEFBuffer, *outNDEFBufferLen);
                    if(0x00 >= res)
                    {
                        LOG("Failed to build handover select message\n");
                    }
                    else
                    {
                        *outNDEFBufferLen = res;
                        res = 0x00;
                    }
                }
            }
            else
            {
                help(0x02);
            }
        }
        else
        {
            LOG("NDEF Type %s not supported\n", type);
            res = 0xFF;
        }
    }
    
    if(NULL != ndef_msg)
    {
        free(ndef_msg);
        ndef_msg = NULL;
        ndef_msg_len = 0x00;
    }
    
    return res;
}

/* if data = NULL this tag is not followed by dataStr : for example -h --help
if format = 0 tag format -t "text" if format=1 tag format : --type=text */
int LookForTag(char** args, int args_len, char* tag, char** data, int format)
{
    int res = 0xFF;
    int i = 0x00;
    int found = 0xFF;
    
    for(i = 0x00; i < args_len; i++)
    {
        found = 0xFF;
        strtolower(args[i]);
        if(0x00 == format)
        {
            found = strcmp(args[i], tag);
        }
        else
        {
            found = strncmp(args[i], tag, strlen(tag));
        }
        
        if(0x00 == found)
        {
            if(NULL != data)
            {
                if(0x00 == format)
                {
                    if(i < (args_len - 1))
                    {
                        *data = args[i + 1];
                        res = 0x00;
                        break;
                    }
                    else
                    {
                        LOG("Argument missing after %s\n", tag);
                    }
                }
                else
                {
                    *data = &args[i][strlen(tag) + 1]; /* +1 to remove '='*/
                    res = 0x00;
                    break;
                }
            }
            else
            {
                res = 0x00;
                break;
            }
        }
    }
    
    return res;
}
 
void cmd_poll(int arg_len, char** arg)
{
    int res = 0x00;
    
    LOG("#########################################################################################\n");
    LOG("##                                       NFC demo                                      ##\n");
    LOG("#########################################################################################\n");
    LOG("##                                 Poll mode activated                                 ##\n");
    LOG("#########################################################################################\n");
    
    InitEnv();
    if(0x00 == LookForTag(arg, arg_len, "-h", NULL, 0x00) || 0x00 == LookForTag(arg, arg_len, "--help", NULL, 0x01))
    {
        help(0x01);
    }
    else
    {
        res = InitMode(0x01, 0x01, 0x00);
        
        if(0x00 == res)
        {
            WaitDeviceArrival(0x01, NULL , 0x00);
        }
    
        res = DeinitPollMode();
    }
    
    LOG("Leaving ...\n");
}
 
void cmd_push(int arg_len, char** arg)
{
    int res = 0x00;
    unsigned char * NDEFMsg = NULL;
    unsigned int NDEFMsgLen = 0x00;
    
    LOG("#########################################################################################\n");
    LOG("##                                       NFC demo                                      ##\n");
    LOG("#########################################################################################\n");
    LOG("##                                 Push mode activated                                 ##\n");
    LOG("#########################################################################################\n");
    
    InitEnv();
    
    if(0x00 == LookForTag(arg, arg_len, "-h", NULL, 0x00) || 0x00 == LookForTag(arg, arg_len, "--help", NULL, 0x01))
    {
        help(0x02);
    }
    else
    {
        res = InitMode(0x01, 0x01, 0x00);
        
        if(0x00 == res)
        {
            res = BuildNDEFMessage(arg_len, arg, &NDEFMsg, &NDEFMsgLen);
        }
        
        if(0x00 == res)
        {
            WaitDeviceArrival(0x02, NDEFMsg, NDEFMsgLen);
        }
        
        if(NULL != NDEFMsg)
        {
            free(NDEFMsg);
            NDEFMsg = NULL;
            NDEFMsgLen = 0x00;
        }
        
        res = DeinitPollMode();
    }
    
    LOG("Leaving ...\n");
}

void cmd_share(int arg_len, char** arg)
{
    int res = 0x00;
    unsigned char * NDEFMsg = NULL;
    unsigned int NDEFMsgLen = 0x00;
    
    LOG("#########################################################################################\n");
    LOG("##                                       NFC demo                                      ##\n");
    LOG("#########################################################################################\n");
    LOG("##                                 Share mode activated                                ##\n");
    LOG("#########################################################################################\n");
    
    InitEnv();
    
    if(0x00 == LookForTag(arg, arg_len, "-h", NULL, 0x00) || 0x00 == LookForTag(arg, arg_len, "--help", NULL, 0x01))
    {
        help(0x02);
    }
    else
    {
        res = InitMode(0x00, 0x00, 0x01);
        
        if(0x00 == res)
        {
            res = BuildNDEFMessage(arg_len, arg, &NDEFMsg, &NDEFMsgLen);
        }
        
        if(0x00 == res)
        {
            T4T_NDEF_EMU_SetRecord(NDEFMsg, NDEFMsgLen, NULL);
        }
        
        if(0x00 == res)
        {
            WaitDeviceArrival(0x04, NDEFMsg, NDEFMsgLen);
        }
        
        if(NULL != NDEFMsg)
        {
            free(NDEFMsg);
            NDEFMsg = NULL;
            NDEFMsgLen = 0x00;
        }
        
        res = DeinitPollMode();
    }
    
    LOG("Leaving ...\n");
}
void cmd_write(int arg_len, char** arg)
{
    int res = 0x00;
    unsigned char * NDEFMsg = NULL;
    unsigned int NDEFMsgLen = 0x00;
    
    LOG("#########################################################################################\n");
    LOG("##                                       NFC demo                                      ##\n");
    LOG("#########################################################################################\n");
    LOG("##                                 Write mode activated                                ##\n");
    LOG("#########################################################################################\n");
    
    InitEnv();
    
    if(0x00 == LookForTag(arg, arg_len, "-h", NULL, 0x00) || 0x00 == LookForTag(arg, arg_len, "--help", NULL, 0x01))
    {
        help(0x02);
    }
    else
    {
        res = InitMode(0x01, 0x01, 0x00);
        
        if(0x00 == res)
        {
            res = BuildNDEFMessage(arg_len, arg, &NDEFMsg, &NDEFMsgLen);
        }
        
        if(0x00 == res)
        {
            WaitDeviceArrival(0x03, NDEFMsg, NDEFMsgLen);
        }
        
        if(NULL != NDEFMsg)
        {
            free(NDEFMsg);
            NDEFMsg = NULL;
            NDEFMsgLen = 0x00;
        }
        
        res = DeinitPollMode();
    }
    
    LOG("Leaving ...\n");
}

void* ExitThread(void* pContext)
{
    LOG("                              ... press Ctrl-C to quit ...\n\n");
    
    // this caused trouble when backgrounding application
    //getchar();
    return;
    
    framework_LockMutex(g_SnepClientLock);
    
    if(eSnepClientState_WAIT_OFF == g_SnepClientState || eSnepClientState_WAIT_READY == g_SnepClientState)
    {
        g_SnepClientState = eSnepClientState_EXIT;
        framework_NotifyMutex(g_SnepClientLock, 0);
    }
    else
    {
        g_SnepClientState = eSnepClientState_EXIT;
    }
    framework_UnlockMutex(g_SnepClientLock);
    
    framework_LockMutex(g_devLock);
    
    if(eDevState_WAIT_ARRIVAL == g_DevState || eDevState_WAIT_DEPARTURE == g_DevState)
    {
        g_DevState = eDevState_EXIT;
        framework_NotifyMutex(g_devLock, 0);
    }
    else
    {
        g_DevState = eDevState_EXIT;
    }
    
    framework_UnlockMutex(g_devLock);
    return NULL;
}

int InitEnv()
{
    eResult tool_res = FRAMEWORK_SUCCESS;
    int res = 0x00;
    
    tool_res = framework_CreateMutex(&g_devLock);
    if(FRAMEWORK_SUCCESS != tool_res)
    {
        res = 0xFF;
    }
    
    if(0x00 == res)
    {
        tool_res = framework_CreateMutex(&g_SnepClientLock);
        if(FRAMEWORK_SUCCESS != tool_res)
        {
            res = 0xFF;
        }
     }
     
     if(0x00 == res)
    {
        tool_res = framework_CreateMutex(&g_HCELock);
        if(FRAMEWORK_SUCCESS != tool_res)
        {
            res = 0xFF;
        }
     }
     if(0x00 == res)
    {
        tool_res = framework_CreateThread(&g_ThreadHandle, ExitThread, NULL);
        if(FRAMEWORK_SUCCESS != tool_res)
        {
            res = 0xFF;
        }
    }
    return res;
}

int CleanEnv()
{
    if(NULL != g_ThreadHandle)
    {
        framework_JoinThread(g_ThreadHandle);
        framework_DeleteThread(g_ThreadHandle);
        g_ThreadHandle = NULL;
    }
        
    if(NULL != g_devLock)
    {
        framework_DeleteMutex(g_devLock);
        g_devLock = NULL;
    }
    
    if(NULL != g_SnepClientLock)
    {
        framework_DeleteMutex(g_SnepClientLock);
        g_SnepClientLock = NULL;
    }
    if(NULL != g_HCELock)
    {
        framework_DeleteMutex(g_HCELock);
        g_HCELock = NULL;
    }
    return 0x00;
}
 
int main(int argc, char ** argv)
{
    char *outputFormat = NULL;
    char *outputFile = NULL;

    if(0x00 == LookForTag(argv, argc, "-x", &outputFormat, 0x01) || 0x00 == LookForTag(argv, argc, "--format", &outputFormat, 0x01))
    {
        if (strcmp(outputFormat, "json") == 0) {
            LOG("Requested JSON output format\n");
            g_OutputFormat = eOutputFormat_JSON;
        } else {
            LOG("Using default output format (TXT)\n");
            g_OutputFormat = eOutputFormat_TXT;
        }
    }

    if(0x00 == LookForTag(argv, argc, "-o", &outputFile, 0x01) || 0x00 == LookForTag(argv, argc, "--output", &outputFile, 0x01))
    {
        if ((g_OutputFile = fopen(outputFile, "w")) == NULL)
        {
            LOG("Failed opening output file");
            return ~0;
        }
        memset(g_jsonBuf, 0, sizeof(g_jsonBuf));
    }

    if(0x00 == LookForTag(argv, argc, "-b", NULL, 0x00) || 0x00 == LookForTag(argv, argc, "--beep", NULL, 0x00))
        g_doBeep = 1;

    if (argc<2)
    {
        LOG("Missing argument\n");
        help(0x00);
    }
    else if (strcmp(argv[1],"poll") == 0)
    {
        cmd_poll(argc - 2, &argv[2]);
    }
    else if(strcmp(argv[1],"write") == 0)
    {
        cmd_write(argc - 2, &argv[2]);
    }
    else if(strcmp(argv[1],"push") == 0)
    {
        cmd_push(argc - 2, &argv[2]);
    }
    else if(strcmp(argv[1],"share") == 0)
    {
        cmd_share(argc - 2, &argv[2]);
    }
    else
    {
        help(0x00);
    }
    LOG("\n");
    
    CleanEnv();

    return 0;
}
 
void help(int mode)
{
    LOG("\nCOMMAND: \n");
    LOG("\tpoll\tPolling mode \t e.g. <nfcDemoApp poll >\n");
    LOG("\twrite\tWrite tag \t e.g. <nfcDemoApp write --type=Text -l en -r \"Test\">\n");
    LOG("\tshare\tTag Emulation Mode \t e.g. <nfcDemoApp share -t URI -u http://www.nxp.com>\n");
    LOG("\tpush\tPush to device \t e.g. <nfcDemoApp push -t URI -u http://www.nxp.com>\n");
    LOG("\t    \t               \t e.g. <nfcDemoApp push --type=mime -m \"application/vnd.bluetooth.ep.oob\" -d \"2200AC597405AF1C0E0947616C617879204E6F74652033040D0C024005031E110B11\">\n");
    LOG("\n");
    LOG("Help Options:\n");
    LOG("-h, --help                       Show help options\n");
    LOG("-x, --format                     Format: 'json' or 'txt' (default) \n");
    LOG("-o, --outfile                    Output file\n");
    LOG("\n");
    
    if(0x01 == mode)
    {
        LOG("No Application Options for poll mode\n");
    }
    if(0x02 == mode)
    {
        LOG("Supported NDEF Types : \n");
        LOG("\t URI\n");
        LOG("\t TEXT\n");
        LOG("\t MIME\n");
        LOG("\t HS (handover select)\n");
        LOG("\n");
        
        LOG("Application Options : \n");
        LOG("\t-l,  --lang=en                   Language\n");
        LOG("\t-m,  --mime=audio/mp3            Mime-type\n");
        LOG("\t-r,  --rep=sample text           Representation\n");
        LOG("\t-t,  --type=Text                 Record type (Text, URI...\n");
        LOG("\t-u,  --uri=http://www.nxp.com    URI\n");
        LOG("\t-d,  --data=00223344565667788    NDEF Data (for type MIME & Handover select)\n");
        LOG("\t-cs, --carrierState=active       Carrier State (for type Handover select)\n");
        LOG("\t-cn, --carrierName=ble           Carrier Reference Name (for type Handover select)\n");
    }
    LOG("\n");
}
