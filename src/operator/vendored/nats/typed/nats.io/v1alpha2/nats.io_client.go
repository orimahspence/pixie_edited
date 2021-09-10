// Code generated by client-gen. DO NOT EDIT.

package v1alpha2

import (
	rest "k8s.io/client-go/rest"
	v1alpha2 "px.dev/pixie/src/operator/apis/nats.io/v1alpha2"
	"px.dev/pixie/src/operator/vendored/nats/scheme"
)

type NatsV1alpha2Interface interface {
	RESTClient() rest.Interface
	NatsClustersGetter
	NatsServiceRolesGetter
}

// NatsV1alpha2Client is used to interact with features provided by the nats.io group.
type NatsV1alpha2Client struct {
	restClient rest.Interface
}

func (c *NatsV1alpha2Client) NatsClusters(namespace string) NatsClusterInterface {
	return newNatsClusters(c, namespace)
}

func (c *NatsV1alpha2Client) NatsServiceRoles(namespace string) NatsServiceRoleInterface {
	return newNatsServiceRoles(c, namespace)
}

// NewForConfig creates a new NatsV1alpha2Client for the given config.
func NewForConfig(c *rest.Config) (*NatsV1alpha2Client, error) {
	config := *c
	if err := setConfigDefaults(&config); err != nil {
		return nil, err
	}
	client, err := rest.RESTClientFor(&config)
	if err != nil {
		return nil, err
	}
	return &NatsV1alpha2Client{client}, nil
}

// NewForConfigOrDie creates a new NatsV1alpha2Client for the given config and
// panics if there is an error in the config.
func NewForConfigOrDie(c *rest.Config) *NatsV1alpha2Client {
	client, err := NewForConfig(c)
	if err != nil {
		panic(err)
	}
	return client
}

// New creates a new NatsV1alpha2Client for the given RESTClient.
func New(c rest.Interface) *NatsV1alpha2Client {
	return &NatsV1alpha2Client{c}
}

func setConfigDefaults(config *rest.Config) error {
	gv := v1alpha2.SchemeGroupVersion
	config.GroupVersion = &gv
	config.APIPath = "/apis"
	config.NegotiatedSerializer = scheme.Codecs.WithoutConversion()

	if config.UserAgent == "" {
		config.UserAgent = rest.DefaultKubernetesUserAgent()
	}

	return nil
}

// RESTClient returns a RESTClient that is used to communicate
// with API server by this client implementation.
func (c *NatsV1alpha2Client) RESTClient() rest.Interface {
	if c == nil {
		return nil
	}
	return c.restClient
}
