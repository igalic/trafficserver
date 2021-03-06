/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/*****************************************************************************
 *
 *  IPAllow.cc - Implementation to IP Access Control systtem
 *
 *
 ****************************************************************************/

#include "libts.h"
#include "Main.h"
#include "IPAllow.h"
#include "ProxyConfig.h"
#include "StatSystem.h"
#include "P_EventSystem.h"
#include "P_Cache.h"
#include "hdrs/HdrToken.h"

#include <sstream>

#define IPAllowRegisterConfigUpdateFunc REC_RegisterConfigUpdateFunc
#define IPAllowReadConfigStringAlloc REC_ReadConfigStringAlloc

enum AclOp {
  ACL_OP_ALLOW, ///< Allow access.
  ACL_OP_DENY, ///< Deny access.
};

IpAllow* IpAllow::_instance = NULL;

// Mask for all methods.
// This can't be computed properly at process start, so it's delayed
// until the instance is initialized.
uint32_t IpAllow::ALL_METHOD_MASK;

static Ptr<ProxyMutex> ip_reconfig_mutex;

//
// struct IPAllow_FreerContinuation
// Continuation to free old cache control lists after
//  a timeout
//
struct IPAllow_FreerContinuation;
typedef int (IPAllow_FreerContinuation::*IPAllow_FrContHandler) (int, void *);
struct IPAllow_FreerContinuation: public Continuation
{
  IpAllow *p;
  int freeEvent(int event, Event * e)
  {
    NOWARN_UNUSED(event);
    NOWARN_UNUSED(e);
    Debug("ip-allow", "Deleting old table");
    delete p;
    delete this;
    return EVENT_DONE;
  }
  IPAllow_FreerContinuation(IpAllow * ap):Continuation(NULL), p(ap)
  {
    SET_HANDLER((IPAllow_FrContHandler) & IPAllow_FreerContinuation::freeEvent);
  }
};

// struct IPAllow_UpdateContinuation
//
//   Used to read the ip_allow.conf file after the manager signals
//      a change
//
struct IPAllow_UpdateContinuation: public Continuation
{
  int file_update_handler(int etype, void *data)
  {
    NOWARN_UNUSED(etype);
    NOWARN_UNUSED(data);
    IpAllow::ReloadInstance();
    delete this;
      return EVENT_DONE;
  }
  IPAllow_UpdateContinuation(ProxyMutex * m):Continuation(m)
  {
    SET_HANDLER(&IPAllow_UpdateContinuation::file_update_handler);
  }
};

int
ipAllowFile_CB(const char *name, RecDataT data_type, RecData data, void *cookie)
{
  NOWARN_UNUSED(name);
  NOWARN_UNUSED(data_type);
  NOWARN_UNUSED(data);
  NOWARN_UNUSED(cookie);
  eventProcessor.schedule_imm(NEW(new IPAllow_UpdateContinuation(ip_reconfig_mutex)), ET_CACHE);
  return 0;
}

//
//   Begin API functions
//
void
IpAllow::InitInstance() {
  // Should not have been initialized before
  ink_assert(_instance == NULL);

  ALL_METHOD_MASK = ~0;

  ip_reconfig_mutex = new_ProxyMutex();

  _instance = NEW(new self("proxy.config.cache.ip_allow.filename", "IpAllow", "ip_allow"));
  _instance->BuildTable();

  IPAllowRegisterConfigUpdateFunc("proxy.config.cache.ip_allow.filename", ipAllowFile_CB, NULL);
}

void
IpAllow::ReloadInstance() {
  self *new_table;

  Debug("ip-allow", "ip_allow.config updated, reloading");

  // Schedule the current table for deallocation in the future
  eventProcessor.schedule_in(NEW(new IPAllow_FreerContinuation(_instance)), IP_ALLOW_TIMEOUT, ET_CACHE);

  new_table = NEW(new self("proxy.config.cache.ip_allow.filename", "IpAllow", "ip_allow"));
  new_table->BuildTable();

  ink_atomic_swap(&_instance, new_table);
}

//
//   End API functions
//


IpAllow::IpAllow(
  const char *config_var,
  const char *name,
  const char *action_val
) : config_file_var(config_var),
    module_name(name),
    action(action_val)
{

  char *config_file;

  config_file_var = ats_strdup(config_var);
  config_file_path[0] = '\0';

  IPAllowReadConfigStringAlloc(config_file, (char *) config_file_var);
  ink_release_assert(config_file != NULL);
  ink_filepath_make(config_file_path, sizeof(config_file_path), system_config_directory, config_file);
  ats_free(config_file);
}

IpAllow::~IpAllow()
{
}

void
IpAllow::Print() {
  std::ostringstream s;
  s << _map.getCount() << " ACL entries";
  s << '.';
  for ( IpMap::iterator spot(_map.begin()), limit(_map.end())
      ; spot != limit
      ; ++spot
  ) {
    char text[INET6_ADDRSTRLEN];
    AclRecord const* ar = static_cast<AclRecord const*>(spot->data());

    s << std::endl << "  Line " << ar->_src_line << ": "
      << ats_ip_ntop(spot->min(), text, sizeof text)
      ;
    if (0 != ats_ip_addr_cmp(spot->min(), spot->max())) {
      s << " - " << ats_ip_ntop(spot->max(), text, sizeof text);
    }
    s << " method=";
    uint32_t mask = ALL_METHOD_MASK & ar->_method_mask;
    if (ALL_METHOD_MASK == mask) {
      s << "ALL";
    } else if (0 == mask) {
      s << "NONE";
    } else {
      bool leader = false; // need leading vbar?
      uint32_t test_mask = 1; // mask for current method.
      for ( int i = 0 ; i < HTTP_WKSIDX_METHODS_CNT ; ++i, test_mask<<=1 ) {
        if (mask & test_mask) {
          if (leader)
            s << '|';
          s << hdrtoken_index_to_wks(i + HTTP_WKSIDX_CONNECT);
          leader = true;
        }
      }
    }
  }
  Debug("ip-allow", "%s", s.str().c_str());
}

int
IpAllow::BuildTable()
{
  char *tok_state = NULL;
  char *line = NULL;
  const char *errPtr = NULL;
  char errBuf[1024];
  char *file_buf = NULL;
  int line_num = 0;
  IpEndpoint addr1;
  IpEndpoint addr2;
  matcher_line line_info;
  bool alarmAlready = false;

  // Table should be empty
  ink_assert(_map.getCount() == 0);

  file_buf = readIntoBuffer(config_file_path, module_name, NULL);

  if (file_buf == NULL) {
    Warning("%s Failed to read %s. All IP Addresses will be blocked", module_name, config_file_path);
    return 1;
  }

  line = tokLine(file_buf, &tok_state);
  while (line != NULL) {

    ++line_num;

    // skip all blank spaces at beginning of line
    while (*line && isspace(*line)) {
      line++;
    }

    if (*line != '\0' && *line != '#') {

      errPtr = parseConfigLine(line, &line_info, &ip_allow_tags);

      if (errPtr != NULL) {
        snprintf(errBuf, sizeof(errBuf), "%s discarding %s entry at line %d : %s",
                 module_name, config_file_path, line_num, errPtr);
        SignalError(errBuf, alarmAlready);
      } else {

        ink_assert(line_info.type == MATCH_IP);

        errPtr = ExtractIpRange(line_info.line[1][line_info.dest_entry], &addr1.sa, &addr2.sa);

        if (errPtr != NULL) {
          snprintf(errBuf, sizeof(errBuf), "%s discarding %s entry at line %d : %s",
                   module_name, config_file_path, line_num, errPtr);
          SignalError(errBuf, alarmAlready);
        } else {
          // INKqa05845
          // Search for "action=ip_allow method=PURGE method=GET ..." or "action=ip_deny method=PURGE method=GET ...".
          char *label, *val;
          uint32_t acl_method_mask = 0;
          AclOp op = ACL_OP_DENY; // "shut up", I explained to the compiler.
          bool op_found = false, method_found = false;
          for (int i = 0; i < MATCHER_MAX_TOKENS; i++) {
            label = line_info.line[0][i];
            val = line_info.line[1][i];
            if (label == NULL) {
              continue;
            }
            if (strcasecmp(label, "action") == 0) {
              if (strcasecmp(val, "ip_allow") == 0) {
                op_found = true, op = ACL_OP_ALLOW;
              } else if (strcasecmp(val, "ip_deny") == 0) {
                op_found = true, op = ACL_OP_DENY;
              }
            }
          }
          if (op_found) {
            // Loop again for methods, (in case action= appears after method=)
            for (int i = 0; i < MATCHER_MAX_TOKENS; i++) {
              label = line_info.line[0][i];
              val = line_info.line[1][i];
              if (label == NULL) {
                continue;
              }
              if (strcasecmp(label, "method") == 0) {
                char *method_name, *sep_ptr = 0;
                // Parse method="GET|HEAD"
                for (method_name = strtok_r(val, "|", &sep_ptr); method_name != NULL; method_name = strtok_r(NULL, "|", &sep_ptr)) {
                  if (strcasecmp(method_name, "ALL") == 0) {
                    method_found = false;  // in case someone does method=GET|ALL
                    break;
                  } else {
                    int method_idx = hdrtoken_tokenize(method_name, strlen(method_name));
                    if (method_idx < HTTP_WKSIDX_CONNECT || method_idx >= HTTP_WKSIDX_CONNECT + HTTP_WKSIDX_METHODS_CNT) {
                      Warning("Method name '%s' on line %d is not valid. Ignoring.", method_name, line_num);
                    } else { // valid method.
                      method_found = true;
                      acl_method_mask |= MethodIdxToMask(method_idx);
                    }
                  }
                }
              }
            }
            // If method not specified, default to ALL
            if (!method_found) {
              method_found = true, acl_method_mask = ALL_METHOD_MASK;
            }
            // When deny, use bitwise complement.  (Make the rule 'allow for all
            // methods except those specified')
            if (op == ACL_OP_DENY) {
              acl_method_mask = ALL_METHOD_MASK & ~acl_method_mask;
            }
          }

          if (method_found) {
            _acls.push_back(AclRecord(acl_method_mask, line_num));
            // Color with index because at this point the address
            // is volatile.
            _map.fill(
              &addr1, &addr2,
              reinterpret_cast<void*>(_acls.length()-1)
            );
          } else {
            snprintf(errBuf, sizeof(errBuf), "%s discarding %s entry at line %d : %s", module_name, config_file_path, line_num, "Invalid action/method specified");        //changed by YTS Team, yamsat bug id -59022
            SignalError(errBuf, alarmAlready);
          }
        }
      }
    }

    line = tokLine(NULL, &tok_state);
  }

  if (_map.getCount() == 0) {
    Warning("%s No entries in %s. All IP Addresses will be blocked", module_name, config_file_path);
  } else {
    // convert the coloring from indices to pointers.
    for ( IpMap::iterator spot(_map.begin()), limit(_map.end())
        ; spot != limit
        ; ++spot
    ) {
      spot->setData(&_acls[reinterpret_cast<size_t>(spot->data())]);
    }
  }

  if (is_debug_tag_set("ip-allow")) {
    Print();
  }

  ats_free(file_buf);
  return 0;
}
