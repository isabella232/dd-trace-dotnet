using System.Data.SqlClient;
using System.Linq;
using Datadog.Core.Tools;
using Datadog.Trace.Configuration;
using Datadog.Trace.TestHelpers;
using Xunit;
using Xunit.Abstractions;

namespace Datadog.Trace.ClrProfiler.IntegrationTests.AdoNet
{
    public class SystemDataSqlClientTests : TestHelper
    {
        public SystemDataSqlClientTests(ITestOutputHelper output)
            : base("SqlServer", output)
        {
            SetServiceVersion("1.0.0");
        }

        [Theory]
        [MemberData(nameof(PackageVersions.SystemDataSqlClient), MemberType = typeof(PackageVersions))]
        [Trait("Category", "EndToEnd")]
        [Trait("RunOnWindows", "True")]
        public void SubmitsTracesWithNetStandard(string packageVersion)
        {
            // Note: The automatic instrumentation currently does not instrument on the generic wrappers
            // due to an issue with constrained virtual method calls. This leads to an inconsistency where
            // the .NET Core apps generate 4 more spans than .NET Framework apps (2 ExecuteReader calls *
            // 2 interfaces: IDbCommand and IDbCommand-netstandard).
            // Once this is fully supported, this will add another 2 complete groups for all frameworks instead
            // of 4 extra spans on net461 and netcoreapp2.0+
#if NET452
            var expectedSpanCount = 49; // 7 queries * 7 groups
#elif NET461
            var expectedSpanCount = 77; // 7 queries * 11 groups
#else
            var expectedSpanCount = 81; // 7 queries * 11 groups + 4 spans from generic wrapper on .NET Core
#endif

            const string dbType = "sql-server";
            const string expectedOperationName = dbType + ".query";
            const string expectedServiceName = "Samples.SqlServer-" + dbType;

            // NOTE: opt into the additional instrumentation of calls into netstandard.dll
            SetEnvironmentVariable("DD_TRACE_NETSTANDARD_ENABLED", "true");

            int agentPort = TcpPortProvider.GetOpenPort();

            using (var agent = new MockTracerAgent(agentPort))
            using (ProcessResult processResult = RunSampleAndWaitForExit(agent.Port, packageVersion: packageVersion))
            {
                Assert.True(processResult.ExitCode >= 0, $"Process exited with code {processResult.ExitCode}");

                var spans = agent.WaitForSpans(expectedSpanCount, operationName: expectedOperationName);
                Assert.Equal(expectedSpanCount, spans.Count);

                foreach (var span in spans)
                {
                    Assert.Equal(expectedOperationName, span.Name);
                    Assert.Equal(expectedServiceName, span.Service);
                    Assert.Equal(SpanTypes.Sql, span.Type);
                    Assert.Equal(dbType, span.Tags[Tags.DbType]);
                    Assert.False(span.Tags?.ContainsKey(Tags.Version), "External service span should not have service version tag.");
                }
            }
        }

        [Fact]
        [Trait("Category", "EndToEnd")]
        [Trait("RunOnWindows", "True")]
        public void SpansDisabledByAdoNetExcludedTypes()
        {
            var totalSpanCount = 21;

            const string dbType = "sql-server";
            const string expectedOperationName = dbType + ".query";

            SetEnvironmentVariable(ConfigurationKeys.AdoNetExcludedTypes, "System.Data.SqlClient.SqlCommand;Microsoft.Data.SqlClient.SqlCommand;MySql.Data.MySqlClient.MySqlCommand;Npgsql.NpgsqlCommand");

            string packageVersion = PackageVersions.SystemDataSqlClient.First()[0] as string;
            int agentPort = TcpPortProvider.GetOpenPort();

            using (var agent = new MockTracerAgent(agentPort))
            using (ProcessResult processResult = RunSampleAndWaitForExit(agent.Port, packageVersion: packageVersion))
            {
                Assert.True(processResult.ExitCode >= 0, $"Process exited with code {processResult.ExitCode}");

                var spans = agent.WaitForSpans(totalSpanCount, returnAllOperations: true);
                Assert.NotEmpty(spans);
                Assert.Empty(spans.Where(s => s.Name.Equals(expectedOperationName)));
            }
        }
    }
}
