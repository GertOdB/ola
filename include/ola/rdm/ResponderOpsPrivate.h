/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ResponderOps.h
 * A framework for building RDM responders.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef INCLUDE_OLA_RDM_RESPONDEROPSPRIVATE_H_
#define INCLUDE_OLA_RDM_RESPONDEROPSPRIVATE_H_

#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>
#include <ola/rdm/RDMCommand.h>
#include <ola/rdm/RDMControllerInterface.h>
#include <ola/rdm/RDMResponseCodes.h>
#include <ola/stl/STLUtils.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ola {
namespace rdm {

using std::auto_ptr;

template <class Target>
ResponderOps<Target>::ResponderOps(const ParamHandler param_handlers[]) {
  // We install placeholders for any pids which are handled internally.
  struct InternalParamHandler placeholder = {NULL, NULL};
  STLReplace(&m_handlers, PID_SUPPORTED_PARAMETERS, placeholder);

  const ParamHandler *handler = param_handlers;
  while (handler->pid && (handler->get_handler || handler->set_handler)) {
    struct InternalParamHandler pid_handler = {
      handler->get_handler,
      handler->set_handler
    };
    STLReplace(&m_handlers, handler->pid, pid_handler);
    handler++;
  }
}

template <class Target>
void ResponderOps<Target>::HandleRDMRequest(Target *target,
                                            const UID &target_uid,
                                            uint16_t sub_device,
                                            const RDMRequest *raw_request,
                                            RDMCallback *on_complete) {
  // Take ownership of the request object, so the targets don't have to.
  auto_ptr<const RDMRequest> request(raw_request);
  vector<string> packets;

  if (!on_complete) {
    OLA_WARN << "Null callback passed!";
    return;
  }

  // If this isn't directed to our UID (unicast, vendorcast or broadcast), we
  // return early.
  if (!request->DestinationUID().DirectedToUID(target_uid)) {
    if (!request->DestinationUID().IsBroadcast()) {
      OLA_WARN << "Received request for the wrong UID, "
               << "expected " << target_uid << ", got "
               << request->DestinationUID();
    }

    on_complete->Run(
        (request->DestinationUID().IsBroadcast() ?
         RDM_WAS_BROADCAST : RDM_TIMEOUT),
        NULL, packets);
    return;
  }

  // Right now we don't support discovery.
  if (request->CommandClass() == RDMCommand::DISCOVER_COMMAND) {
    on_complete->Run(RDM_PLUGIN_DISCOVERY_NOT_SUPPORTED, NULL, packets);
    return;
  }

  // broadcast GETs are noops.
  if (request->CommandClass() == RDMCommand::GET_COMMAND &&
      request->DestinationUID().IsBroadcast()) {
    OLA_WARN << "Received broadcast GET command";
    on_complete->Run(RDM_WAS_BROADCAST, NULL, packets);
    return;
  }

  const RDMResponse *response = NULL;
  rdm_response_code response_code = RDM_COMPLETED_OK;

  // Right now we don't support sub devices
  bool for_our_subdevice = request->SubDevice() == sub_device ||
                           request->SubDevice() == ALL_RDM_SUBDEVICES;

  if (!for_our_subdevice) {
    if (request->DestinationUID().IsBroadcast()) {
      on_complete->Run(RDM_WAS_BROADCAST, NULL, packets);
    } else {
      response = NackWithReason(request.get(), NR_SUB_DEVICE_OUT_OF_RANGE);
      on_complete->Run(RDM_COMPLETED_OK, response, packets);
    }
    return;
  }

  // gets to ALL_RDM_SUBDEVICES are a special case
  if (request->SubDevice() == ALL_RDM_SUBDEVICES &&
      request->CommandClass() == RDMCommand::GET_COMMAND) {
    // the broadcast get case was handled above.
    response = NackWithReason(request.get(), NR_SUB_DEVICE_OUT_OF_RANGE);
    on_complete->Run(RDM_COMPLETED_OK, response, packets);
    return;
  }

  InternalParamHandler *handler = STLFind(&m_handlers, request->ParamId());
  if (!handler) {
    if (request->DestinationUID().IsBroadcast()) {
      on_complete->Run(RDM_WAS_BROADCAST, NULL, packets);
    } else {
      response = NackWithReason(request.get(), NR_UNKNOWN_PID);
      on_complete->Run(RDM_COMPLETED_OK, response, packets);
    }
    return;
  }

  if (request->CommandClass() == RDMCommand::GET_COMMAND) {
    if (request->DestinationUID().IsBroadcast()) {
      // this should have been handled above, but be safe.
      response_code = RDM_WAS_BROADCAST;
    } else {
      if (handler->get_handler) {
        response = (target->*(handler->get_handler))(request.get());
      } else {
        switch (request->ParamId()) {
          case PID_SUPPORTED_PARAMETERS:
            response = HandleSupportedParams(request.get());
            break;
          default:
            response = NackWithReason(request.get(),
                                      NR_UNSUPPORTED_COMMAND_CLASS);
        }
      }
    }
  } else if (request->CommandClass() == RDMCommand::SET_COMMAND) {
    if (handler->set_handler) {
      response = (target->*(handler->set_handler))(request.get());
    } else {
      response = NackWithReason(request.get(), NR_UNSUPPORTED_COMMAND_CLASS);
    }
  }

  if (request->DestinationUID().IsBroadcast()) {
    if (response) {
      delete response;
    }
    on_complete->Run(RDM_WAS_BROADCAST, NULL, packets);
  } else {
    on_complete->Run(response_code, response, packets);
  }
}

template <class Target>
RDMResponse *ResponderOps<Target>::HandleSupportedParams(
    const RDMRequest *request) {
  if (request->ParamDataSize())
    return NackWithReason(request, NR_FORMAT_ERROR);

  vector<uint16_t> params;
  params.reserve(m_handlers.size());
  typename RDMHandlers::const_iterator iter = m_handlers.begin();
  for (; iter != m_handlers.end(); ++iter) {
    uint16_t pid = iter->first;
    // some pids never appear in supported_parameters.
    if (pid != PID_SUPPORTED_PARAMETERS &&
        pid != PID_PARAMETER_DESCRIPTION &&
        pid != PID_DEVICE_INFO &&
        pid != PID_SOFTWARE_VERSION_LABEL &&
        pid != PID_DMX_START_ADDRESS &&
        pid != PID_IDENTIFY_DEVICE) {
      params.push_back(iter->first);
    }
  }
  sort(params.begin(), params.end());

  vector<uint16_t>::iterator param_iter = params.begin();
  for (; param_iter != params.end(); ++param_iter) {
    *param_iter = ola::network::HostToNetwork(*param_iter);
  }

  return GetResponseFromData(
      request,
      reinterpret_cast<uint8_t*>(&params[0]),
      params.size() * sizeof(uint16_t));
}
}  // namespace rdm
}  // namespace ola
#endif  // INCLUDE_OLA_RDM_RESPONDEROPSPRIVATE_H_