#define EXTENSION_NAME AdMob
#define MODULE_NAME "admob"
#define LIB_NAME "AdMob"

// Defold SDK
#define DLIB_LOG_DOMAIN LIB_NAME
#include <dmsdk/sdk.h>

#if defined(DM_PLATFORM_IOS) || defined(DM_PLATFORM_ANDROID)

// Firebase sdk ref:
// https://firebase.google.com/docs/reference/cpp/namespace/firebase/admob
// https://firebase.google.com/docs/reference/cpp/struct/firebase/admob/ad-request
// https://firebase.google.com/docs/admob/cpp/rewarded-video


#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "firebase/admob.h"
#include "firebase/admob/banner_view.h"
#include "firebase/admob/interstitial_ad.h"
#include "firebase/admob/native_express_ad_view.h"
#include "firebase/admob/rewarded_video.h"
#include "firebase/admob/types.h"
#include "firebase/app.h"
#include "firebase/future.h"


typedef void (*PostCommandFn)(int id);
static void QueueCommand(int id, int message, int firebase_result, const char* firebase_message, PostCommandFn fn);
static void ClearAdRequest(firebase::admob::AdRequest& adrequest);

namespace
{

enum AdMobAdType
{
    ADMOB_TYPE_INVALID = 0,
    ADMOB_TYPE_BANNER,
    ADMOB_TYPE_INTERSTITIAL,
    ADMOB_TYPE_VIDEO,
};

enum AdMobError
{
    ADMOB_ERROR_NONE                = firebase::admob::kAdMobErrorNone,
    ADMOB_ERROR_UNINITIALIZED       = firebase::admob::kAdMobErrorUninitialized,
    ADMOB_ERROR_ALREADYINITIALIZED  = firebase::admob::kAdMobErrorAlreadyInitialized,
    ADMOB_ERROR_LOADINPROGRESS      = firebase::admob::kAdMobErrorLoadInProgress,
    ADMOB_ERROR_INTERNALERROR       = firebase::admob::kAdMobErrorInternalError,
    ADMOB_ERROR_INVALIDREQUEST      = firebase::admob::kAdMobErrorInvalidRequest,
    ADMOB_ERROR_NETWORKERROR        = firebase::admob::kAdMobErrorNetworkError,
    ADMOB_ERROR_NOFILL              = firebase::admob::kAdMobErrorNoFill,
    ADMOB_ERROR_NOWINDOWTOKEN       = firebase::admob::kAdMobErrorNoWindowToken,
};

// https://firebase.google.com/docs/reference/cpp/namespace/firebase/admob#namespacefirebase_1_1admob_1a1908085a11fb74e08dd24b1ec5b019ec
enum AdMobChildResquestTreatmentState
{
    ADMOB_CHILDDIRECTED_TREATMENT_STATE_NOT_TAGGED  = firebase::admob::kChildDirectedTreatmentStateNotTagged,
    ADMOB_CHILDDIRECTED_TREATMENT_STATE_TAGGED      = firebase::admob::kChildDirectedTreatmentStateTagged,
    ADMOB_CHILDDIRECTED_TREATMENT_STATE_UNKNOWN     = firebase::admob::kChildDirectedTreatmentStateUnknown
};

enum AdMobGender
{
    ADMOB_GENDER_UNKNOWN        = firebase::admob::kGenderUnknown,
    ADMOB_GENDER_FEMALE         = firebase::admob::kGenderFemale,
    ADMOB_GENDER_MALE           = firebase::admob::kGenderMale,
};

enum AdMobPosition
{
    ADMOB_POSITION_TOP          = firebase::admob::BannerView::kPositionTop,
    ADMOB_POSITION_BOTTOM       = firebase::admob::BannerView::kPositionBottom,
    ADMOB_POSITION_TOPLEFT      = firebase::admob::BannerView::kPositionTopLeft,
    ADMOB_POSITION_TOPRIGHT     = firebase::admob::BannerView::kPositionTopRight,
    ADMOB_POSITION_BOTTOMLEFT   = firebase::admob::BannerView::kPositionBottomLeft,
    ADMOB_POSITION_BOTTOMRIGHT  = firebase::admob::BannerView::kPositionBottomRight
};

enum AdMobEvent
{
    ADMOB_MESSAGE_LOADED,
    ADMOB_MESSAGE_FAILED_TO_LOAD,
    ADMOB_MESSAGE_SHOW,
    ADMOB_MESSAGE_HIDE,
    ADMOB_MESSAGE_APP_LEAVE,
    ADMOB_MESSAGE_UNLOADED,
};

struct LuaCallbackInfo
{
    LuaCallbackInfo() : m_L(0), m_Callback(LUA_NOREF), m_Self(LUA_NOREF) {}
    lua_State* m_L;
    int        m_Callback;
    int        m_Self;
};

struct AdMobState;

class BannerViewListener : public firebase::admob::BannerView::Listener
{
public:
    BannerViewListener(AdMobState* state, int id) : m_State(state), m_Id(id) {}
    void OnBoundingBoxChanged(firebase::admob::BannerView* banner_view, firebase::admob::BoundingBox box) {
        (void)banner_view;
        (void)box;
    }
    void OnPresentationStateChanged(firebase::admob::BannerView* banner_view, firebase::admob::BannerView::PresentationState state);

    AdMobState* m_State;
    int         m_Id;       // The internal ad number
};

struct MessageCommand
{
    PostCommandFn m_PostFn; // A function to be called after the command was processed
    char* m_FirebaseMessage;
    int m_Id;
    int m_Message;
    int m_FirebaseResult;
};

class InterstitialAdListener : public firebase::admob::InterstitialAd::Listener
{
public:
    InterstitialAdListener(AdMobState* state, int id) : m_State(state), m_Id(id) {}
    void OnPresentationStateChanged(firebase::admob::InterstitialAd* interstitial_ad, firebase::admob::InterstitialAd::PresentationState state);

    AdMobState* m_State;
    int m_Id; // The internal ad number
};


struct AdMobDelayedDelete
{
    firebase::admob::BannerView*  m_BannerView;
    BannerViewListener*           m_BannerViewListener;
};

static void OnDestroyHideCallback(const firebase::Future<void>& future, void* user_data);


struct AdMobAd
{
    AdMobAdType                 m_Type;
    firebase::admob::AdRequest  m_AdRequest;
    LuaCallbackInfo             m_Callback;
    const char*                 m_AdUnit;
    uint8_t                     m_Initialized : 1;

    // Set to non zero depending on ad type
    firebase::admob::BannerView*        m_BannerView;
    firebase::admob::InterstitialAd*    m_InterstitialAd;
    // Listeners
    BannerViewListener*           m_BannerViewListener;
    InterstitialAdListener*       m_InterstitialAdListener;

    AdMobAd()
    {
        memset(this, 0, sizeof(*this));
        m_Callback.m_Callback = LUA_NOREF;
        m_Callback.m_Self = LUA_NOREF;
    }

    void Clear()
    {
        ClearAdRequest(m_AdRequest);

dmLogWarning("CLEAR: m_BannerView: %p  m_InterstitialAd: %p  m_BannerViewListener: %p  m_InterstitialAdListener: %p", m_BannerView, m_InterstitialAd, m_BannerViewListener, m_InterstitialAdListener);

        if( m_BannerView != 0 )
        {
            //m_BannerView->SetListener(0);

            AdMobDelayedDelete* info = new AdMobDelayedDelete;
            info->m_BannerView          = m_BannerView;
            info->m_BannerViewListener  = m_BannerViewListener;
            //::firebase::Future< void > future = m_BannerView->Destroy();
            //m_BannerView->DestroyLastResult().OnCompletion( OnDestroyedCallback, m_BannerViewListener );

            // while(future.status() == firebase::kFutureStatusPending) {
            //     dmLogWarning("ping");
            // }

dmLogWarning("CLEAR: info: %p m_BannerView: %p  m_BannerViewListener: %p ", info, m_BannerView, m_BannerViewListener);

            //m_BannerView->Hide();
            //m_BannerView->HideLastResult().OnCompletion( OnDestroyHideCallback, this );
            //m_BannerView->HideLastResult().OnCompletion( OnDestroyHideCallback, info );
            m_BannerView->Hide().OnCompletion( OnDestroyHideCallback, info );
            //delete m_BannerView;
        }
        if( m_InterstitialAd != 0 )
        {
            m_InterstitialAd->SetListener(0);
            delete m_InterstitialAd;

            if( m_InterstitialAdListener )
                delete m_InterstitialAdListener;
        }
        if( m_AdUnit )
            free((void*)m_AdUnit);
        
        memset(this, 0, sizeof(AdMobAd));
        m_Callback.m_Callback = LUA_NOREF;
        m_Callback.m_Self = LUA_NOREF;
    }

    void Show()
    {
        if(m_BannerView)
            m_BannerView->Show();
        else if(m_InterstitialAd)
            m_InterstitialAd->Show();
    }
    void Hide()
    {
        if(m_BannerView)
            m_BannerView->Hide();
    }
    void Pause()
    {
        if(m_BannerView)
            m_BannerView->Pause();
    }
    void Resume()
    {
        if(m_BannerView)
            m_BannerView->Resume();
    }
};

const int ADMOB_MAX_ADS = 16;

struct AdMobState
{
    firebase::App*  m_App;
    AdMobAd         m_Ads[ADMOB_MAX_ADS];
    int             m_CoveringUIAd;         // Which ad went fullscreen?

    dmArray<MessageCommand> m_CmdQueue;
};

static void OnDestroyHideCallback(const firebase::Future<void>& future, void* user_data)
{
    //::firebase::admob::BannerView* banner_view = (::firebase::admob::BannerView*)user_data;
//dmLogWarning("OnHideDestroyedCallback: BannerView: %p", banner_view);

    AdMobDelayedDelete* info = (AdMobDelayedDelete*)info;
dmLogWarning("OnHideDestroyedCallback: info: %p  BannerView: %p  BannerViewListener: %p", info, info->m_BannerView, info->m_BannerViewListener);

    delete info->m_BannerView;
    delete info->m_BannerViewListener;

    /*
    AdMobAd* ad = (AdMobAd*)user_data;

dmLogWarning("OnHideDestroyedCallback: BannerViewListener: %p", ad);
    if( ad )
    {

dmLogWarning("OnHideDestroyedCallback: deleted %p  %p", ad->m_BannerView, ad->m_BannerViewListener);

        delete ad->m_BannerView;
        delete ad->m_BannerViewListener;
    }*/
}



void BannerViewListener::OnPresentationStateChanged(firebase::admob::BannerView* banner_view, firebase::admob::BannerView::PresentationState state)
{
    dmLogWarning("    BannerViewListener :: OnPresentationStateChanged: id: %d  state: %d", m_Id, state);

    if( state == firebase::admob::BannerView::kPresentationStateCoveringUI ) // When clicked
    {
        if( m_State->m_CoveringUIAd == -1 ) // Because the state change gets triggered twice
        {
            m_State->m_CoveringUIAd = m_Id;
            QueueCommand(m_Id, ADMOB_MESSAGE_APP_LEAVE, 0, 0, 0);
        }
    }
    else if( state == firebase::admob::BannerView::kPresentationStateHidden )
    {
        QueueCommand(m_Id, ADMOB_MESSAGE_HIDE, 0, 0, 0);
    }
    else if( state == firebase::admob::BannerView::kPresentationStateVisibleWithAd )
    {
        QueueCommand(m_Id, ADMOB_MESSAGE_SHOW, 0, 0, 0);
    }
}

void InterstitialAdListener::OnPresentationStateChanged(firebase::admob::InterstitialAd* interstitial_ad, firebase::admob::InterstitialAd::PresentationState state)
{
    dmLogWarning("    InterstitialAdListener :: OnPresentationStateChanged: id: %d  state: %d", m_Id, state);

    // When showing ad, it also leaves the app
    if( state == firebase::admob::InterstitialAd::kPresentationStateCoveringUI )
    {
        QueueCommand(m_Id, ADMOB_MESSAGE_SHOW, 0, 0, 0);
        if(m_State->m_CoveringUIAd == -1)
        {
            m_State->m_CoveringUIAd = m_Id;
            QueueCommand(m_Id, ADMOB_MESSAGE_APP_LEAVE, 0, 0, 0);
        }
    }
    else if( state == firebase::admob::InterstitialAd::kPresentationStateHidden )
    {
        QueueCommand(m_Id, ADMOB_MESSAGE_HIDE, 0, 0, 0);
    }
}

} // namespace

::AdMobState* g_AdMob = 0;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LUA helpers

static void ClearAdRequest(firebase::admob::AdRequest& adrequest)
{
    for( uint32_t i = 0; i < adrequest.keyword_count; ++i)
    {
        free((void*)adrequest.keywords[i]);
    }
    if(adrequest.keywords != 0)
    {
        free((void*)adrequest.keywords);
        adrequest.keywords = 0;
    }

    for( uint32_t i = 0; i < adrequest.test_device_id_count; ++i)
    {
        free((void*)adrequest.test_device_ids[i]);
    }
    if(adrequest.test_device_ids != 0)
    {
        free((void*)adrequest.test_device_ids);
        adrequest.test_device_ids = 0;
    }

    for( uint32_t i = 0; i < adrequest.extras_count; ++i)
    {
        free((void*)adrequest.extras[i].key);
        free((void*)adrequest.extras[i].value);
    }
    if(adrequest.extras != 0)
    {
        free((void*)adrequest.extras);
        adrequest.extras = 0;
    }
    memset(&adrequest, 0, sizeof(adrequest));
}

// http://www.defold.com/ref/dmScript/#dmScript::GetMainThread
static void RegisterCallback(lua_State* L, int index, LuaCallbackInfo* cbk)
{
    if(cbk->m_Callback != LUA_NOREF)
    {
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Callback);
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Self);
    }

    cbk->m_L = dmScript::GetMainThread(L);
    luaL_checktype(L, index, LUA_TFUNCTION);

    lua_pushvalue(L, index);
    cbk->m_Callback = dmScript::Ref(L, LUA_REGISTRYINDEX);

    dmScript::GetInstance(L);
    cbk->m_Self = dmScript::Ref(L, LUA_REGISTRYINDEX);

dmLogWarning("RegisterCallback END");
}

static void UnregisterCallback(LuaCallbackInfo* cbk)
{
    if(cbk->m_Callback != LUA_NOREF)
    {
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Callback);
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Self);
        cbk->m_Callback = LUA_NOREF;
    }
}

static void InvokeCallback(LuaCallbackInfo* cbk, int id, int message, int result, const char* result_message)
{
    if(cbk->m_Callback == LUA_NOREF)
    {
        return;
    }

    lua_State* L = cbk->m_L;
    DM_LUA_STACK_CHECK(L, 0);

    lua_rawgeti(L, LUA_REGISTRYINDEX, cbk->m_Callback);

    // Setup self (the script instance)
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbk->m_Self);
    lua_pushvalue(L, -1);

    dmScript::SetInstance(L);

    ::AdMobAd* ad = &g_AdMob->m_Ads[id];

    lua_pushnumber(L, id);

    lua_newtable(L);

        lua_pushnumber(L, ad->m_Type);
        lua_setfield(L, -2, "type");

        lua_pushstring(L, ad->m_AdUnit);
        lua_setfield(L, -2, "ad_unit");

        lua_pushnumber(L, message);
        lua_setfield(L, -2, "message");

        lua_pushnumber(L, result);
        lua_setfield(L, -2, "result");

        lua_pushstring(L, result_message ? result_message : "");
        lua_setfield(L, -2, "result_string");

    int number_of_arguments = 3; // instance + 2
    int ret = lua_pcall(L, number_of_arguments, 0, 0);
    if(ret != 0) {
        dmLogError("Error running callback: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

// Gets a number (or a default value) from a table
static int CheckTableNumber(lua_State* L, int index, const char* name, int default_value)
{
    DM_LUA_STACK_CHECK(L, 0);

    int result = -1;
    lua_pushstring(L, name);
    lua_gettable(L, index);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return default_value;
    }
    else if (lua_isnumber(L, -1)) {
        result = lua_tointeger(L, -1);
    } else {
        return DM_LUA_ERROR("Wrong type for table attribute '%s'. Expected number, got %s", name, luaL_typename(L, -1));
    }
    lua_pop(L, 1);
    return result;
}

// Gets a list of strings from a table
static void CheckTableStringList(lua_State* L, int index, char*** outlist, uint32_t* length )
{
    DM_LUA_STACK_CHECK(L, 0);
    if( !lua_istable(L, index) )
    {
        *outlist = 0;
        *length = 0;
        return;
    }
    
    int len = lua_objlen(L, index);
    char** list = (char**)malloc(sizeof(char*) * len);

    lua_pushvalue(L, index); // push table
    lua_pushnil(L);  // first key

    int i = 0;
    while (lua_next(L, -2) != 0)
    {
        const char* s = lua_tostring(L, -1);
        if (!s) {
            DM_LUA_ERROR("Wrong type for table attribute '%s'. Expected string, got %s", lua_tostring(L, -2), luaL_typename(L, -1) );
            return;
        }
        list[i++] = strdup(s);

        // removes 'value'; keeps 'key' for next iteration
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop table

    *outlist = list;
    *length = (uint32_t)len;
}


// Gets a list of strings from a table
static void CheckTableKeyValueList(lua_State* L, int index, firebase::admob::KeyValuePair** outlist, uint32_t* length )
{
    DM_LUA_STACK_CHECK(L, 0);
    if( !lua_istable(L, index) )
    {
        *outlist = 0;
        *length = 0;
        return;
    }
    
    int len = lua_objlen(L, index);
    firebase::admob::KeyValuePair* list = (firebase::admob::KeyValuePair*)malloc(sizeof(firebase::admob::KeyValuePair) * len);

    lua_pushvalue(L, index); // push table
    lua_pushnil(L);  // first key

    int i = 0;
    while (lua_next(L, -2) != 0)
    {
        const char* k = lua_tostring(L, -2);
        const char* s = lua_tostring(L, -1);
        if (!s) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Wrong type for table attribute '%s'. Expected string, got %s", lua_tostring(L, -2), luaL_typename(L, -1) );
            luaL_error(L, msg);
            return;
        }
        list[i].key = strdup(k);
        list[i].value = strdup(s);
        i++;

        // removes 'value'; keeps 'key' for next iteration
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop table

    *outlist = list;
    *length = (uint32_t)len;
}

static void SetupAdRequest(lua_State* L, int index, firebase::admob::AdRequest& adrequest)
{
    DM_LUA_STACK_CHECK(L, 0);

    memset(&adrequest, 0, sizeof(adrequest));

    adrequest.birthday_day = CheckTableNumber(L, index, "birthday_day", 1);
    adrequest.birthday_month = CheckTableNumber(L, index, "birthday_month", 1);
    adrequest.birthday_year = CheckTableNumber(L, index, "birthday_year", 1970);
    adrequest.gender = (firebase::admob::Gender)CheckTableNumber(L, index, "gender", ADMOB_GENDER_UNKNOWN);
    adrequest.tagged_for_child_directed_treatment = (firebase::admob::ChildDirectedTreatmentState)CheckTableNumber(L, index,
                                                            "tagged_for_child_directed_treatment", ADMOB_CHILDDIRECTED_TREATMENT_STATE_NOT_TAGGED);

    lua_pushvalue(L, index); // push table
    lua_pushnil(L);  // first key
    while (lua_next(L, -2) != 0)
    {
        const char* key = lua_tostring(L, -2);

        if( strcmp("keywords", key) == 0 ) {
            CheckTableStringList(L, -1, (char***)&adrequest.keywords, &adrequest.keyword_count);
        }
        else if( strcmp("testdevices", key) == 0 ) {
            CheckTableStringList(L, -1, (char***)&adrequest.test_device_ids, &adrequest.test_device_id_count);
        }
        else if( strcmp("extras", key) == 0 ) {
            CheckTableKeyValueList(L, -1, (firebase::admob::KeyValuePair**)&adrequest.extras, &adrequest.extras_count);
        }

        // removes 'value'; keeps 'key' for next iteration
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop table
}


static bool IsIdValid(int id)
{
    return id >= 0 && id < ADMOB_MAX_ADS;
}

static int FindNewSlot()
{
    for( uint32_t i = 0; i < ADMOB_MAX_ADS; ++i)
    {
        if( g_AdMob->m_Ads[i].m_Type == ADMOB_TYPE_INVALID )
            return i;
    }
    return -1;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////



static void QueueCommand(int id, int message, int firebase_result, const char* firebase_message, PostCommandFn fn)
{
    MessageCommand cmd;
    cmd.m_Id = id;
    cmd.m_Message = message;
    cmd.m_FirebaseResult = firebase_result;
    cmd.m_FirebaseMessage = firebase_message ? strdup(firebase_message) : 0;
    cmd.m_PostFn = fn;

    // TODO: Add mutex here
    if(g_AdMob->m_CmdQueue.Full())
    {
        g_AdMob->m_CmdQueue.OffsetCapacity(8);
    }
    g_AdMob->m_CmdQueue.Push(cmd);
}

static void FlushCommandQueue()
{
    for(uint32_t i = 0; i != g_AdMob->m_CmdQueue.Size(); ++i)
    {
        MessageCommand* cmd = &g_AdMob->m_CmdQueue[i];
        ::AdMobAd& ad = g_AdMob->m_Ads[cmd->m_Id];

        InvokeCallback(&ad.m_Callback, cmd->m_Id, cmd->m_Message, cmd->m_FirebaseResult, cmd->m_FirebaseMessage ? cmd->m_FirebaseMessage : "");

        if( cmd->m_PostFn )
            cmd->m_PostFn(cmd->m_Id);

        if( cmd->m_FirebaseMessage )
            free(cmd->m_FirebaseMessage);
        cmd->m_FirebaseMessage = 0;

        g_AdMob->m_CmdQueue.EraseSwap(i--);
    }
}


static void CleanCommandCallback(int id)
{
    ::AdMobAd* ad = &g_AdMob->m_Ads[id];
    UnregisterCallback(&ad->m_Callback);
    ad->Clear();
}


static void OnLoadedCallback(const firebase::Future<void>& future, void* user_data)
{
    ::AdMobAd* ad = (AdMobAd*)user_data;
    int id = (int)(uintptr_t)(ad - &g_AdMob->m_Ads[0]);

    int result = future.error();

dmLogWarning("OnLoadedCallback: %d id: %d  type: %d", __LINE__, id, ad->m_Type);

    if (result != firebase::admob::kAdMobErrorNone)
    {
        QueueCommand(id, ADMOB_MESSAGE_FAILED_TO_LOAD, future.error(), future.error_message(), CleanCommandCallback);
        return;
    }

    ad->m_Initialized = 1;
    if( ad->m_Type == ADMOB_TYPE_BANNER )
    {
        ad->m_BannerViewListener = new ::BannerViewListener(g_AdMob, id);
        ad->m_BannerView->SetListener(ad->m_BannerViewListener);
    }
    else if( ad->m_Type == ADMOB_TYPE_INTERSTITIAL )
    {
        ad->m_InterstitialAdListener = new ::InterstitialAdListener(g_AdMob, id);
        ad->m_InterstitialAd->SetListener(ad->m_InterstitialAdListener);
    }

    QueueCommand(id, ADMOB_MESSAGE_LOADED, future.error(), future.error_message(), 0);
}

static void OnCompletionCallback(const firebase::Future<void>& future, void* user_data)
{
    ::AdMobAd* ad = (AdMobAd*)user_data;
    int id = (int)(uintptr_t)(ad - &g_AdMob->m_Ads[0]);

    dmLogWarning("OnCompletionCallback: %d id: %d  type: %d", __LINE__, id, ad->m_Type);

    //ad->m_Result = future.error();
    if (future.error() == firebase::admob::kAdMobErrorNone)
    {
dmLogWarning("OnCompletionCallback: %d id: %d  ad: %p  banner: %p  interstitial: %p", __LINE__, id, ad, ad->m_BannerView, ad->m_InterstitialAd);

        switch(ad->m_Type)
        {
        case ADMOB_TYPE_BANNER:         ad->m_BannerView->LoadAd(ad->m_AdRequest);
                                        ad->m_BannerView->LoadAdLastResult().OnCompletion(OnLoadedCallback, ad);
                                        return;

        case ADMOB_TYPE_INTERSTITIAL:   ad->m_InterstitialAd->LoadAd(ad->m_AdRequest);
                                        ad->m_InterstitialAd->LoadAdLastResult().OnCompletion(OnLoadedCallback, ad);
                                        return;
        default:
            break;
        }
    }

    QueueCommand(id, ADMOB_MESSAGE_FAILED_TO_LOAD, future.error(), future.error_message(), CleanCommandCallback);
}




//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Glue functions

#if defined(DM_PLATFORM_IOS)
static inline firebase::admob::AdParent GetAdParent()
{
    return (firebase::admob::AdParent)(id)dmGraphics::GetNativeiOSUIView();
}
#else
static inline firebase::admob::AdParent GetAdParent()
{
    return (firebase::admob::AdParent)(jobject)dmGraphics::GetNativeAndroidActivity();
}
static JNIEnv* GetJNIEnv()
{
    JNIEnv* env = 0;
    dmGraphics::GetNativeAndroidJavaVM()->AttachCurrentThread(&env, NULL);
    return env;
}
#endif

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Lua implementation

static int Load(lua_State* L)
{
    if (!g_AdMob) {
        return luaL_error(L, "AdMob not initialized.");
    }

    int id = FindNewSlot();
    if( !IsIdValid(id) ) {
        return luaL_error(L, "Too many ads in use: %d", ADMOB_MAX_ADS);
    }

    DM_LUA_STACK_CHECK(L, 1);

    int ad_type = luaL_checkint(L, 1);
    if( !(ad_type == ADMOB_TYPE_BANNER || ad_type == ADMOB_TYPE_INTERSTITIAL) ) {
        return luaL_error(L, "Unexpected ad type: %d", ad_type);
    }

    const char* ad_unit = luaL_checkstring(L, 2);

    ::AdMobAd* ad = &g_AdMob->m_Ads[id];
    ad->m_AdUnit = strdup( ad_unit );

    SetupAdRequest(L, 3, ad->m_AdRequest);

    RegisterCallback(L, 4, &ad->m_Callback);

    ad->m_Type = (::AdMobAdType)ad_type;
    if( ad_type == ADMOB_TYPE_BANNER )
    {
        firebase::admob::AdSize ad_size;
        ad_size.ad_size_type = firebase::admob::kAdSizeStandard;
        ad_size.width = CheckTableNumber(L, 3, "width", 320);
        ad_size.height = CheckTableNumber(L, 3, "height", 100);

        ad->m_BannerView = new firebase::admob::BannerView();
        ad->m_BannerView->Initialize(GetAdParent(), ad->m_AdUnit, ad_size);
        ad->m_BannerView->InitializeLastResult().OnCompletion(OnCompletionCallback, ad);
    }
    else if( ad_type == ADMOB_TYPE_INTERSTITIAL )
    {
        ad->m_InterstitialAd = new firebase::admob::InterstitialAd();
        ad->m_InterstitialAd->Initialize(GetAdParent(), ad->m_AdUnit);
        ad->m_InterstitialAd->InitializeLastResult().OnCompletion(OnCompletionCallback, ad);
    }

    lua_pushnumber(L, id);
    return 1;
}


static int Show(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 0);
    int id = luaL_checkint(L, 1);
    if( !IsIdValid(id) )
        return luaL_error(L, "Invalid id: %d", id);

    g_AdMob->m_Ads[id].Show();
    return 0;
}

static int Hide(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 0);
    int id = luaL_checkint(L, 1);
    if( !IsIdValid(id) )
        return luaL_error(L, "Invalid id: %d", id);

    g_AdMob->m_Ads[id].Hide();
    return 0;
}

static int MoveTo(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 0);

    int id = luaL_checkint(L, 1);
    if( !IsIdValid(id) )
        return luaL_error(L, "Invalid id: %d", id);

    ::AdMobAd* ad = &g_AdMob->m_Ads[id];

    if(ad->m_Type != ADMOB_TYPE_BANNER)
        return luaL_error(L, "move_to is only supported by banner ads (id: %d  type: %d)", id, ad->m_Type);
    
    if(ad->m_Initialized == 0)
        return luaL_error(L, "move_to can only be called after initialization is done (id: %d  type: %d)", id, ad->m_Type);

    if(lua_gettop(L) == 1)
    {
        int _pos = luaL_checkint(L, 1);
        if( _pos < firebase::admob::BannerView::kPositionTop || _pos > firebase::admob::BannerView::kPositionBottomRight )
            return luaL_error(L, "Invalid position: %d", _pos);

        firebase::admob::BannerView::Position pos = (firebase::admob::BannerView::Position)_pos;
        ad->m_BannerView->MoveTo(pos);
    }
    else
    {
        int x = luaL_checkint(L, 2);
        int y = luaL_checkint(L, 3);
        ad->m_BannerView->MoveTo(x, y);
    }

    return 0;
}

static int Unload(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 0);
    int id = luaL_checkint(L, 1);
    if( !IsIdValid(id) )
        return luaL_error(L, "Invalid id: %d", id);

    QueueCommand(id, ADMOB_MESSAGE_UNLOADED, 0, 0, CleanCommandCallback);
    return 0;
}


static const luaL_reg Module_methods[] =
{
    {"load", Load},
    {"show", Show},
    {"hide", Hide},
    {"move_to", MoveTo},
    {"unload", Unload},
    {0, 0}
};

static void LuaInit(lua_State* L)
{
    int top = lua_gettop(L);
    luaL_register(L, MODULE_NAME, Module_methods);

#define SETCONSTANT(name) \
        lua_pushnumber(L, (lua_Number) ADMOB_ ## name); \
        lua_setfield(L, -2, #name);\

    SETCONSTANT(ERROR_NONE);
    SETCONSTANT(ERROR_UNINITIALIZED);
    SETCONSTANT(ERROR_ALREADYINITIALIZED);
    SETCONSTANT(ERROR_LOADINPROGRESS);
    SETCONSTANT(ERROR_INTERNALERROR);
    SETCONSTANT(ERROR_INVALIDREQUEST);
    SETCONSTANT(ERROR_NETWORKERROR);
    SETCONSTANT(ERROR_NOFILL);
    SETCONSTANT(ERROR_NOWINDOWTOKEN);

    SETCONSTANT(TYPE_BANNER);
    SETCONSTANT(TYPE_INTERSTITIAL);
    SETCONSTANT(TYPE_VIDEO);

    SETCONSTANT(CHILDDIRECTED_TREATMENT_STATE_NOT_TAGGED);
    SETCONSTANT(CHILDDIRECTED_TREATMENT_STATE_TAGGED);
    SETCONSTANT(CHILDDIRECTED_TREATMENT_STATE_UNKNOWN);

    SETCONSTANT(GENDER_UNKNOWN);
    SETCONSTANT(GENDER_FEMALE);
    SETCONSTANT(GENDER_MALE);

    SETCONSTANT(POSITION_TOP);
    SETCONSTANT(POSITION_BOTTOM);
    SETCONSTANT(POSITION_TOPLEFT);
    SETCONSTANT(POSITION_TOPRIGHT);
    SETCONSTANT(POSITION_BOTTOMLEFT);
    SETCONSTANT(POSITION_BOTTOMRIGHT);

    SETCONSTANT(MESSAGE_LOADED);
    SETCONSTANT(MESSAGE_FAILED_TO_LOAD);
    SETCONSTANT(MESSAGE_SHOW);
    SETCONSTANT(MESSAGE_HIDE);
    SETCONSTANT(MESSAGE_APP_LEAVE);
    SETCONSTANT(MESSAGE_UNLOADED);

#undef SETCONSTANT

    lua_pop(L, 1);
    assert(top == lua_gettop(L));
}

///////////////////////////////////////////////////////////////////////////////////////////////
// Extension interface functions

static dmExtension::Result AppInitializeExtension(dmExtension::AppParams* params)
{
    if (g_AdMob) {
        dmLogError("AdMob already initialized.");
        return dmExtension::RESULT_OK;
    }

#if defined(__ANDROID__)
    const char* app_id = dmConfigFile::GetString(params->m_ConfigFile, "admob.app_id_android", 0);
#else
    const char* app_id = dmConfigFile::GetString(params->m_ConfigFile, "admob.app_id_ios", 0);
#endif
    if( !app_id )
    {
        dmLogError("No admob.app_id set in game.project!");
        return dmExtension::RESULT_OK;
    }

#if defined(__ANDROID__)
    firebase::App* app = ::firebase::App::Create(::firebase::AppOptions(), GetJNIEnv(), dmGraphics::GetNativeAndroidActivity());
#else
    firebase::App* app = ::firebase::App::Create(::firebase::AppOptions());
#endif

    if(!app)
    {
        dmLogError("::firebase::App::Create failed");
        return dmExtension::RESULT_OK;
    }

    firebase::InitResult res = firebase::admob::Initialize(*app, app_id);
    if (res != firebase::kInitResultSuccess)
    {
        delete app;
        dmLogError("Could not initialize AdMob, result: %d", res);
        return dmExtension::RESULT_OK;
    }

    g_AdMob = new ::AdMobState;
    g_AdMob->m_App = app;
    g_AdMob->m_CoveringUIAd = -1;
    g_AdMob->m_CmdQueue.SetCapacity(8);

    dmLogInfo("AdMob fully initialized!");

    return dmExtension::RESULT_OK;
}

static dmExtension::Result InitializeExtension(dmExtension::Params* params)
{
    LuaInit(params->m_L);
    dmLogInfo("Registered %s Lua extension\n", MODULE_NAME);
    return dmExtension::RESULT_OK;
}

static dmExtension::Result AppFinalizeExtension(dmExtension::AppParams* params)
{
    if( !g_AdMob )
        return dmExtension::RESULT_OK;

    for( uint32_t i = 0; i < ADMOB_MAX_ADS; ++i)
    {
        g_AdMob->m_Ads[i].Clear();
    }

    if(g_AdMob->m_App)
    {
        firebase::admob::Terminate();
        //firebase::admob::rewarded_video::Destroy();
        delete g_AdMob->m_App;
    }

    delete g_AdMob;
    g_AdMob = 0;
    return dmExtension::RESULT_OK;
}

static dmExtension::Result FinalizeExtension(dmExtension::Params* params)
{
    return dmExtension::RESULT_OK;
}

static dmExtension::Result UpdateExtension(dmExtension::Params* params)
{
    if( g_AdMob )
        FlushCommandQueue();
    return dmExtension::RESULT_OK;
}

static void OnEventExtension(dmExtension::Params* params, const dmExtension::Event* event)
{
    if( !g_AdMob )
        return;
    if( event->m_Event == dmExtension::EVENT_ID_ACTIVATEAPP )
    {
        g_AdMob->m_CoveringUIAd = -1;
        for( uint32_t i = 0; i < ADMOB_MAX_ADS; ++i)
        {
            g_AdMob->m_Ads[i].Resume();
        }
    }
    else if(event->m_Event == dmExtension::EVENT_ID_DEACTIVATEAPP)
    {
        if( g_AdMob->m_CoveringUIAd != -1 )
        {
            //QueueCommand(g_AdMob->m_CoveringUIAd, ADMOB_MESSAGE_APP_LEAVE, 0, "");
            FlushCommandQueue();
        }

        for( uint32_t i = 0; i < ADMOB_MAX_ADS; ++i)
        {
            g_AdMob->m_Ads[i].Pause();
        }
    }
}

#else // DM_PLATFORM_IOS


static dmExtension::Result AppInitializeExtension(dmExtension::AppParams* params)
{
    dmLogWarning("Registered %s (null) Extension\n", MODULE_NAME);
    return dmExtension::RESULT_OK;
}

static dmExtension::Result InitializeExtension(dmExtension::Params* params)
{
    return dmExtension::RESULT_OK;
}

static dmExtension::Result AppFinalizeExtension(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

static dmExtension::Result FinalizeExtension(dmExtension::Params* params)
{
    return dmExtension::RESULT_OK;
}

static dmExtension::Result UpdateExtension(dmExtension::Params* params)
{
    return dmExtension::RESULT_OK;
}

static void OnEventExtension(dmExtension::Params* params, const dmExtension::Event* event)
{
}

#endif


DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitializeExtension, AppFinalizeExtension, InitializeExtension, UpdateExtension, OnEventExtension, FinalizeExtension)
