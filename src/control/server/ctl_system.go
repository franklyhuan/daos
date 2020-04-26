//
// (C) Copyright 2018-2020 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//

package server

import (
	"strings"
	"time"

	"github.com/pkg/errors"
	"golang.org/x/net/context"

	"github.com/daos-stack/daos/src/control/common/proto/convert"
	ctlpb "github.com/daos-stack/daos/src/control/common/proto/ctl"
	"github.com/daos-stack/daos/src/control/lib/control"
	"github.com/daos-stack/daos/src/control/system"
)

const systemReqTimeout = 30 * time.Second

// systemRanksFunc is an alias for control client API *Ranks() fanout
// function that executes across ranks on different hosts.
type systemRanksFunc func(context.Context, control.UnaryInvoker, *control.RanksReq) (*control.RanksResp, error)

// getMSMember retrieves the MS leader record from the membership.
func (svc *ControlService) getMSMember() (*system.Member, error) {
	if svc.membership == nil {
		return nil, errors.New("host not an access point")
	}

	msInstance, err := svc.harness.GetMSLeaderInstance()
	if err != nil {
		return nil, errors.Wrap(err, "get MS instance")
	}
	if msInstance == nil {
		return nil, errors.New("MS instance not found")
	}

	rank, err := msInstance.GetRank()
	if err != nil {
		return nil, err
	}

	msMember, err := svc.membership.Get(rank)
	if err != nil {
		return nil, errors.WithMessage(err, "retrieving MS member")
	}

	return msMember, nil
}

// resultsFromBadHosts generate synthetic member results for ranks on
// hosts that return errors.
func (svc *ControlService) resultsFromBadHosts(ranks []system.Rank, hostErrors control.HostErrorsMap) system.MemberResults {
	hostRanks := svc.membership.HostRanks(ranks...) // results filtered by input rank list
	results := make(system.MemberResults, 0, len(ranks))

	// synthesise "Stopped" rank results for any harness host errors
	for errMsg, hostSet := range hostErrors {
		for _, addr := range strings.Split(hostSet.DerangedString(), ",") {
			// TODO: should annotate member state with "harness unresponsive" err
			for _, rank := range hostRanks[addr] {
				results = append(results,
					system.NewMemberResult(rank, "", errors.New(errMsg),
						system.MemberStateStopped))
			}
			svc.log.Debugf("harness %s (ranks %v) host error: %s",
				addr, hostRanks[addr], errMsg)
		}
	}

	return results
}

// filterRanks takes a slice of system ranks as a filter and returns rank
// slice with or without MS rank. Include all ranks if input list is empty.
func (svc *ControlService) filterRanks(ranks []system.Rank, excludeMS bool) ([]system.Rank, error) {
	msMember, err := svc.getMSMember()
	if err != nil {
		return nil, errors.WithMessage(err, "retrieving MS member")
	}

	if len(ranks) == 0 {
		ranks = svc.membership.Ranks() // empty rankList implies update all ranks
	}

	if excludeMS {
		ranks = msMember.Rank.RemoveFromList(ranks)
	}

	return ranks, nil
}

// rpcToRanks sends requests to ranks in list on their respective host
// addresses through functions implementing UnaryInvoker.
func (svc *ControlService) rpcToRanks(ctx context.Context, force bool, ranks []system.Rank, fn systemRanksFunc) (system.MemberResults, error) {
	results := make(system.MemberResults, 0, len(ranks))

	req := &control.RanksReq{Ranks: ranks, Force: force}
	// provided ranks should never be empty at this point
	if len(ranks) == 0 {
		return nil, errors.Errorf("no ranks specified in the request: %+v", req)
	}
	req.SetHostList(svc.membership.Hosts(ranks...))
	resp, err := fn(ctx, svc.rpcClient, req)
	if err != nil {
		return nil, err
	}
	results = append(results, svc.resultsFromBadHosts(ranks, resp.HostErrors)...)
	results = append(results, resp.RankResults...)

	return results, nil
}

// resultsFromRanks sends RPCs to all ranks except MS leader and returns the
// relevant system member results.
func (svc *ControlService) resultsFromRanks(ctx context.Context, force bool, rankList []system.Rank, fn systemRanksFunc) (system.MemberResults, error) {
	ranks, err := svc.filterRanks(rankList, true)
	if err != nil {
		return nil, err
	}

	return svc.rpcToRanks(ctx, force, ranks, fn)
}

// resultsFromMSRanks sends RPCs to MS leader ranks and returns the system
// member result.
func (svc *ControlService) getResultsFromMSRank(ctx context.Context, force bool, rankList []system.Rank, fn systemRanksFunc) (system.MemberResults, error) {
	msMember, err := svc.getMSMember()
	if err != nil {
		return nil, errors.WithMessage(err, "retrieving MS member")
	}

	if len(rankList) != 0 && !msMember.Rank.InList(rankList) {
		return nil, nil
	}

	return svc.rpcToRanks(ctx, force, []system.Rank{msMember.Rank}, fn)
}

// pingMembers requests registered harness to ping their instances (system
// members) in order to determine IO Server process responsiveness. Update membership
// appropriately.
//
// Each host address represents a gRPC server associated with a harness managing
// one or more data-plane instances (DAOS system members).
func (svc *ControlService) pingMembers(ctx context.Context, rankList []system.Rank) error {
	ranks, err := svc.filterRanks(rankList, false)
	if err != nil {
		return err
	}

	results, err := svc.rpcToRanks(ctx, false, ranks, control.PingRanks)
	if err != nil {
		return err
	}

	// only update members in the appropriate state (Joined/Stopping)
	// leave unresponsive members to be updated by a join
	filteredMembers := svc.membership.Members(rankList, system.MemberStateEvicted,
		system.MemberStateErrored, system.MemberStateUnknown,
		system.MemberStateStopped, system.MemberStateUnresponsive)

	for _, m := range filteredMembers {
		for _, r := range results {
			// Update either:
			// - members unresponsive to ping
			// - members with stopped processes
			// - members returning errors e.g. from dRPC ping
			if r.State != system.MemberStateUnresponsive &&
				r.State != system.MemberStateStopped &&
				r.State != system.MemberStateErrored {

				continue
			}
			if err := svc.membership.SetMemberState(m.Rank, r.State); err != nil {
				return errors.Wrapf(err, "setting state of rank %d", m.Rank)
			}
		}
	}

	return nil
}

// SystemQuery implements the method defined for the Management Service.
//
// Return status of system members specified in request rank list (or all
// members if request rank list is empty).
func (svc *ControlService) SystemQuery(parent context.Context, req *ctlpb.SystemQueryReq) (*ctlpb.SystemQueryResp, error) {
	svc.log.Debug("Received SystemQuery RPC")

	_, err := svc.harness.GetMSLeaderInstance()
	if err != nil {
		return nil, errors.WithMessage(err, "query requires active MS")
	}

	ctx, cancel := context.WithTimeout(parent, systemReqTimeout)
	defer cancel()

	// Update and retrieve status of system members in rank list.
	rankList := system.RanksFromUint32(req.GetRanks())
	if err := svc.pingMembers(ctx, rankList); err != nil {
		return nil, err
	}
	members := svc.membership.Members(rankList)

	resp := &ctlpb.SystemQueryResp{}
	if err := convert.Types(members, &resp.Members); err != nil {
		return nil, err
	}

	svc.log.Debug("Responding to SystemQuery RPC")

	return resp, nil
}

// prepShutdown requests registered harness to prepare their instances (system members)
// for system shutdown.
//
// Each host address represents a gRPC server associated with a harness managing
// one or more data-plane instances (DAOS system members) present in rankList.
func (svc *ControlService) prepShutdown(ctx context.Context, rankList []system.Rank) (system.MemberResults, error) {
	svc.log.Debug("preparing ranks for shutdown")

	results, err := svc.resultsFromRanks(ctx, false, rankList, control.PrepShutdownRanks)
	if err != nil {
		return nil, err
	}

	msResults, err := svc.getResultsFromMSRank(ctx, false, rankList, control.PrepShutdownRanks)
	if err != nil {
		return nil, err
	}
	results = append(results, msResults...)

	if err := svc.membership.UpdateMemberStates(results); err != nil {
		return nil, err
	}

	for _, result := range results {
		result.Action = "prep shutdown"
	}

	return results, nil
}

// shutdown requests registered harnesses to stop its instances (system members).
//
// Each host address represents a gRPC server associated with a harness managing
// one or more data-plane instances (DAOS system members) present in rankList.
func (svc *ControlService) shutdown(ctx context.Context, force bool, rankList []system.Rank) (system.MemberResults, error) {
	svc.log.Debug("shutting down ranks")

	results, err := svc.resultsFromRanks(ctx, force, rankList, control.StopRanks)
	if err != nil {
		return nil, err
	}

	msResults, err := svc.getResultsFromMSRank(ctx, force, rankList, control.StopRanks)
	if err != nil {
		return nil, err
	}
	results = append(results, msResults...)

	if err := svc.membership.UpdateMemberStates(results); err != nil {
		return nil, err
	}

	for _, result := range results {
		result.Action = "stop"
	}

	return results, nil
}

// SystemStop implements the method defined for the Management Service.
//
// Initiate controlled shutdown of DAOS system, return results for each rank.
func (svc *ControlService) SystemStop(parent context.Context, req *ctlpb.SystemStopReq) (*ctlpb.SystemStopResp, error) {
	svc.log.Debug("Received SystemStop RPC")

	resp := &ctlpb.SystemStopResp{}

	// TODO: consider locking to prevent join attempts when shutting down

	ctx, cancel := context.WithTimeout(parent, systemReqTimeout)
	defer cancel()

	if req.Prep {
		// prepare system members for shutdown
		prepResults, err := svc.prepShutdown(ctx,
			system.RanksFromUint32(req.GetRanks()))
		if err != nil {
			return nil, err
		}
		if err := convert.Types(prepResults, &resp.Results); err != nil {
			return nil, err
		}
		if !req.Force && prepResults.HasErrors() {
			return resp, errors.New("PrepShutdown HasErrors")
		}
	}

	if req.Kill {
		// shutdown by stopping system members
		stopResults, err := svc.shutdown(ctx, req.Force,
			system.RanksFromUint32(req.GetRanks()))
		if err != nil {
			return nil, err
		}
		if err := convert.Types(stopResults, &resp.Results); err != nil {
			return nil, err
		}
	}

	if resp.Results == nil {
		return nil, errors.New("response results not populated")
	}

	svc.log.Debug("Responding to SystemStop RPC")

	return resp, nil
}

// start requests registered harnesses to start their instances (system members)
// after a controlled shutdown using information in the membership registry.
//
// Each host address represents a gRPC server associated with a harness managing
// one or more data-plane instances (DAOS system members).
func (svc *ControlService) start(ctx context.Context, rankList []system.Rank) (system.MemberResults, error) {
	svc.log.Debug("starting ranks")

	results, err := svc.getResultsFromMSRank(ctx, false, rankList, control.StartRanks)
	if err != nil {
		return nil, err
	}

	otherResults, err := svc.resultsFromRanks(ctx, false, rankList, control.StartRanks)
	if err != nil {
		return nil, err
	}
	results = append(results, otherResults...)

	// member state will transition to "Joined" during join or bootstrap,
	// here we want to update membership only if "Errored" or "Ready"
	filteredResults := make(system.MemberResults, 0, len(results))
	for _, r := range results {
		if !r.Errored {
			if r.State != system.MemberStateReady {
				continue
			}
			// don't update members to "Ready" if already "Joined"
			m, err := svc.membership.Get(r.Rank)
			if err != nil {
				return nil, errors.Wrap(err, "result rank not in membership")
			}
			if m.State() == system.MemberStateJoined {
				continue
			}
		}
		filteredResults = append(filteredResults, r)
	}

	if err := svc.membership.UpdateMemberStates(filteredResults); err != nil {
		return nil, err
	}

	for _, result := range results {
		result.Action = "start"
	}

	return results, nil
}

// SystemStart implements the method defined for the Management Service.
//
// Initiate controlled start of DAOS system, return results for each rank.
func (svc *ControlService) SystemStart(parent context.Context, req *ctlpb.SystemStartReq) (*ctlpb.SystemStartResp, error) {
	svc.log.Debug("Received SystemStart RPC")

	ctx, cancel := context.WithTimeout(parent, systemReqTimeout)
	defer cancel()

	// start any stopped system members, note that instances will only
	// be started on hosts with all instances stopped
	startResults, err := svc.start(ctx, system.RanksFromUint32(req.GetRanks()))
	if err != nil {
		return nil, err
	}

	resp := &ctlpb.SystemStartResp{}
	if err := convert.Types(startResults, &resp.Results); err != nil {
		return nil, err
	}

	svc.log.Debug("Responding to SystemStart RPC")

	return resp, nil
}
