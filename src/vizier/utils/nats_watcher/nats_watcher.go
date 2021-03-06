/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package main

import (
	"fmt"
	"time"

	"github.com/fatih/color"
	"github.com/gogo/protobuf/proto"
	"github.com/gogo/protobuf/types"
	"github.com/nats-io/nats.go"
	log "github.com/sirupsen/logrus"
	"github.com/spf13/viper"

	"px.dev/pixie/src/shared/cvmsgspb"
	"px.dev/pixie/src/shared/services"
	"px.dev/pixie/src/vizier/messages/messagespb"
)

func connectNATS() *nats.Conn {
	natsURL := "pl-nats"

	var nc *nats.Conn
	var err error
	if viper.GetBool("disable_ssl") {
		nc, err = nats.Connect(natsURL)
	} else {
		nc, err = nats.Connect(natsURL,
			nats.ClientCert(viper.GetString("client_tls_cert"), viper.GetString("client_tls_key")),
			nats.RootCAs(viper.GetString("tls_ca_cert")))
	}

	if err != nil && viper.GetBool("disable_ssl") {
		log.WithError(err).
			WithField("client_tls_cert", viper.GetString("client_tls_cert")).
			WithField("client_tls_key", viper.GetString("client_tls_key")).
			WithField("tls_ca_cert", viper.GetString("tls_ca_cert")).
			Fatal("Failed to connect to NATS")
	} else if err != nil {
		log.WithError(err).Fatal("Failed to connect to NATS")
	}

	return nc
}

func handleV2CMessage(m *nats.Msg) {
	pb := &cvmsgspb.V2CMessage{}
	err := proto.Unmarshal(m.Data, pb)
	if err != nil {
		log.WithError(err).Error("Failed to unmarshal V2CMessage.")
		return
	}

	if pb.Msg == nil {
		log.Errorf("Invalid msg: %s", proto.MarshalTextString(pb))
	}

	var dyn types.DynamicAny
	if err := types.UnmarshalAny(pb.Msg, &dyn); err != nil {
		log.WithError(err).Error("Failed to unmarshal inner message.")
		return
	}

	innerPb := dyn.Message

	red := color.New(color.FgRed).SprintfFunc()

	fmt.Printf("----------------------------------------------------------\n")
	fmt.Printf("%s=%s\n", red("Subject"), m.Subject)
	fmt.Printf("%s\n", proto.MarshalTextString(innerPb))
	fmt.Printf("----------------------------------------------------------\n")
}

func handleNATSMessage(m *nats.Msg) {
	pb := &messagespb.VizierMessage{}
	err := proto.Unmarshal(m.Data, pb)

	if err != nil {
		log.WithError(err).Error("Failed to Unmarshal proto message.")
	}

	red := color.New(color.FgRed).SprintfFunc()

	fmt.Printf("----------------------------------------------------------\n")
	fmt.Printf("%s=%s\n", red("Subject"), m.Subject)
	fmt.Printf("%s\n", proto.MarshalTextString(pb))
	fmt.Printf("----------------------------------------------------------\n")
}

func main() {
	services.SetupCommonFlags()
	services.SetupSSLClientFlags()
	services.PostFlagSetupAndParse()
	services.CheckSSLClientFlags()

	log.Info("Starting NATS watcher")

	nc := connectNATS()
	defer nc.Close()

	// Listen to all V2C messages.
	if _, err := nc.Subscribe("v2c.*", handleV2CMessage); err != nil {
		log.WithError(err).Fatal("Could not subscribe to NATS")
	}

	// Listen to all messages without prefixes.
	if _, err := nc.Subscribe("*", handleNATSMessage); err != nil {
		log.WithError(err).Fatal("Could not subscribe to NATS")
	}

	// Run forever to maintain subscription.
	for {
		time.Sleep(300 * time.Millisecond)
	}
}
