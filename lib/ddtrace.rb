# typed: strict

# Load tracing
require_relative 'datadog/tracing'
require_relative 'datadog/tracing/contrib'

# Load appsec
require_relative 'datadog/appsec/autoload' # TODO: datadog/appsec?

# Load other products (must follow tracing)
require_relative 'datadog/profiling'
require_relative 'datadog/ci'
require_relative 'datadog/kit'
