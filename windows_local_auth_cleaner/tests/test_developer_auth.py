from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from browser_cleaner.config import BrowserCatalog
from browser_cleaner.developer_auth import CommandResult, CredentialEntry, DeveloperAuthService
from browser_cleaner.models import CleanerOptions
from browser_cleaner.profiles import ProfileFinder
from browser_cleaner.verification import VerificationService


class FakeRunner:
    def __init__(self, unavailable: bool = False) -> None:
        self.unavailable = unavailable
        self.commands: list[tuple[str, ...]] = []

    def run(self, command: tuple[str, ...]) -> CommandResult | None:
        self.commands.append(command)
        if self.unavailable:
            return None
        if command == ("gh", "auth", "status"):
            return CommandResult(0, "github.com\n  ✓ Logged in to github.com account octo-one (keyring)\n  ✓ Logged in to github.com account octo-two (keyring)\n", "")
        if command == ("git", "credential-manager", "github", "list"):
            return CommandResult(0, "username: octo-one\nusername: octo-two\n", "")
        if command[-2:] == ("tunnel", "status"):
            return CommandResult(0, "not registered\n", "")
        if command == ("devtunnel", "user", "show"):
            return CommandResult(0, "Not logged in.\n", "")
        if "logout" in command or command[-2:] == ("tunnel", "unregister"):
            return CommandResult(0, "", "")
        return CommandResult(1, "", "")


class FakeCredentialStore:
    def __init__(self) -> None:
        self.entries = [CredentialEntry("git:https://github.com", "octo-one", 1)]
        self.deleted: list[str] = []

    def list_supported_entries(self):
        return list(self.entries)

    def delete(self, entry):
        self.deleted.append(entry.target_name)
        self.entries.remove(entry)
        return True


class FakeEnvironmentStore:
    def __init__(self) -> None:
        self.names = {"GH_TOKEN", "AZURE_TEST_TOKEN"}
        self.deleted: list[str] = []

    def list_token_names(self):
        return set(self.names)

    def audit_names(self):
        return sorted(self.names | {"GITHUB_TOKEN"})

    def delete(self, name):
        self.deleted.append(name)
        self.names.discard(name)
        return True


class DeveloperAuthTests(unittest.TestCase):
    def setUp(self) -> None:
        documents = patch("browser_cleaner.powershell.PowerShellProfileManager._find_documents", return_value=None)
        documents.start()
        self.addCleanup(documents.stop)
        self.runner = FakeRunner()
        self.credentials = FakeCredentialStore()
        self.environment = FakeEnvironmentStore()
        self.service = DeveloperAuthService(
            runner=self.runner,
            credential_store=self.credentials,
            environment_store=self.environment,
            code_commands=("code",),
            devtunnel_command="devtunnel",
        )

    def test_audit_and_dry_run_handle_multiple_github_accounts_without_secrets(self) -> None:
        entries = self.service.audit()
        github_entries = [entry for entry in entries if entry.provider == "GitHub CLI"]
        self.assertEqual([entry.account_name for entry in github_entries], ["octo-one", "octo-two"])
        # Variable *names* are intentionally audited, but the fixture secret
        # value is neither passed to nor displayed by the service.
        self.assertNotIn("fixture-secret-value", "\n".join(str(entry) for entry in entries))

        results = self.service.remove_supported(False, lambda _subject, _phrase: True)

        self.assertTrue(results)
        self.assertTrue(all(result.status in {"dry-run", "unsupported"} for result in results))
        self.assertEqual(self.credentials.deleted, [])
        self.assertEqual(self.environment.deleted, [])

    def test_apply_uses_official_commands_and_per_item_confirmation(self) -> None:
        confirmations: list[tuple[str, str]] = []

        def confirm(subject: str, phrase: str) -> bool:
            confirmations.append((subject, phrase))
            return True

        results = self.service.remove_supported(True, confirm)

        self.assertTrue(all(result.status == "removed" for result in results))
        self.assertIn(("gh", "auth", "logout", "--hostname", "github.com", "--user", "octo-one"), self.runner.commands)
        self.assertIn(("gh", "auth", "logout", "--hostname", "github.com", "--user", "octo-two"), self.runner.commands)
        self.assertIn(("git", "credential-manager", "github", "logout", "octo-one"), self.runner.commands)
        self.assertIn(("devtunnel", "user", "logout"), self.runner.commands)
        self.assertEqual(self.credentials.deleted, ["git:https://github.com"])
        self.assertEqual(set(self.environment.deleted), {"GH_TOKEN", "AZURE_TEST_TOKEN"})
        self.assertGreaterEqual(len(confirmations), 6)

    def test_missing_commands_are_unsupported_not_fatal(self) -> None:
        service = DeveloperAuthService(
            runner=FakeRunner(unavailable=True),
            credential_store=FakeCredentialStore(),
            environment_store=FakeEnvironmentStore(),
            code_commands=("code",),
            devtunnel_command="devtunnel",
        )

        entries = service.audit()

        self.assertEqual(next(entry for entry in entries if entry.provider == "GitHub CLI").login_status, "unsupported")
        self.assertEqual(next(entry for entry in entries if entry.provider == "Git Credential Manager").login_status, "unsupported")
        self.assertEqual(next(entry for entry in entries if entry.provider == "Microsoft Dev Tunnels").login_status, "unsupported")

    def test_partial_official_logout_failure_does_not_hide_other_results(self) -> None:
        class PartialRunner(FakeRunner):
            def run(self, command):
                if command == ("gh", "auth", "logout", "--hostname", "github.com", "--user", "octo-two"):
                    self.commands.append(command)
                    return CommandResult(1, "", "")
                return super().run(command)

        runner = PartialRunner()
        credentials = FakeCredentialStore()
        environment = FakeEnvironmentStore()
        service = DeveloperAuthService(
            runner=runner,
            credential_store=credentials,
            environment_store=environment,
            code_commands=("code",),
            devtunnel_command="devtunnel",
        )

        results = service.remove_supported(True, lambda _subject, _phrase: True)

        failed = next(result for result in results if result.provider == "GitHub CLI" and result.subject == "octo-two")
        self.assertEqual(failed.status, "error")
        self.assertTrue(any(result.status == "removed" for result in results))
        self.assertEqual(credentials.deleted, ["git:https://github.com"])
        self.assertEqual(set(environment.deleted), {"GH_TOKEN", "AZURE_TEST_TOKEN"})


class DevTunnelRunner(FakeRunner):
    def __init__(
        self,
        initial: CommandResult | None = CommandResult(0, "Logged in as octo using GitHub.\n", ""),
        after_logout: CommandResult | None = CommandResult(0, "Not logged in.\n", ""),
        logout: CommandResult | None = CommandResult(0, "", ""),
    ) -> None:
        super().__init__()
        self.current = initial
        self.after_logout = after_logout
        self.logout = logout

    def run(self, command):
        if command == ("devtunnel", "user", "show"):
            self.commands.append(command)
            return self.current
        if command == ("devtunnel", "user", "logout"):
            self.commands.append(command)
            if self.logout is not None and self.logout.returncode == 0:
                self.current = self.after_logout
            return self.logout
        return super().run(command)


class DevTunnelAuthTests(unittest.TestCase):
    def setUp(self) -> None:
        documents = patch("browser_cleaner.powershell.PowerShellProfileManager._find_documents", return_value=None)
        documents.start()
        self.addCleanup(documents.stop)

    def _service(self, runner):
        return DeveloperAuthService(
            runner=runner,
            credential_store=FakeCredentialStore(),
            environment_store=FakeEnvironmentStore(),
            code_commands=(),
            devtunnel_command="devtunnel",
        )

    def _devtunnel_result(self, service, apply, confirm=lambda _subject, _phrase: True):
        return next(result for result in service.remove_supported(apply, confirm) if result.provider == "Microsoft Dev Tunnels")

    def _verification(self, service):
        verifier = VerificationService(service, BrowserCatalog(), ProfileFinder(), None)
        options = CleanerOptions(False, False, False, "all", categories=("developer-auth",), verify=True)
        return next(entry for entry in verifier.verify(options) if entry.subject == "Microsoft Dev Tunnels")

    def test_dry_run_and_declined_confirmation_do_not_log_out(self):
        runner = DevTunnelRunner()
        service = self._service(runner)
        audit = next(entry for entry in service.audit() if entry.provider == "Microsoft Dev Tunnels")
        self.assertEqual(audit.login_status, "logged in")
        self.assertEqual(self._devtunnel_result(service, False).status, "dry-run")
        self.assertEqual(self._devtunnel_result(service, True, lambda _subject, _phrase: False).status, "manual")
        self.assertNotIn(("devtunnel", "user", "logout"), runner.commands)

    def test_official_logout_is_confirmed_and_verified_without_showing_raw_output(self):
        runner = DevTunnelRunner(initial=CommandResult(0, "Logged in as octo using GitHub.\nfixture-secret-value\n", ""))
        service = self._service(runner)
        audit = service.audit()
        confirmations = []

        def confirm(subject, phrase):
            confirmations.append(phrase)
            return True

        result = self._devtunnel_result(service, True, confirm)
        verified = self._verification(service)

        self.assertEqual(result.status, "removed")
        self.assertIn("LOGOUT DEVTUNNEL", confirmations)
        self.assertIn(("devtunnel", "user", "logout"), runner.commands)
        self.assertEqual(verified.status, "no local evidence found")
        self.assertNotIn("fixture-secret-value", str(audit) + str(result) + str(verified))

    def test_signed_out_cli_still_gets_logout_to_clear_any_expired_cache(self):
        runner = DevTunnelRunner(initial=CommandResult(0, "Not logged in.\n", ""))
        self.assertEqual(self._devtunnel_result(self._service(runner), True).status, "removed")
        self.assertIn(("devtunnel", "user", "logout"), runner.commands)

    def test_missing_cli_is_reported_without_hiding_other_cleanup(self):
        runner = DevTunnelRunner(initial=None)
        service = self._service(runner)
        results = service.remove_supported(True, lambda _subject, _phrase: True)
        self.assertEqual(next(result for result in results if result.provider == "Microsoft Dev Tunnels").status, "unsupported")
        self.assertTrue(any(result.status == "removed" for result in results))
        self.assertEqual(self._verification(service).status, "manual verification required")
        self.assertNotIn(("devtunnel", "user", "logout"), runner.commands)

    def test_logout_failure_does_not_stop_other_providers(self):
        runner = DevTunnelRunner(logout=CommandResult(1, "", "fixture-secret-value"))
        results = self._service(runner).remove_supported(True, lambda _subject, _phrase: True)
        self.assertEqual(next(result for result in results if result.provider == "Microsoft Dev Tunnels").status, "error")
        self.assertTrue(any(result.status == "removed" for result in results))
        self.assertNotIn("fixture-secret-value", str(results))

    def test_successful_logout_is_not_enough_when_login_remains_or_status_is_unknown(self):
        for outcome, expected in (
            (CommandResult(0, "Logged in as octo using GitHub.\n", ""), "error"),
            (CommandResult(1, "", "network error"), "manual"),
            (CommandResult(1, "Not logged in.\n", "permission denied"), "manual"),
            (CommandResult(0, "", ""), "manual"),
            (CommandResult(0, "unrecognized localized status", ""), "manual"),
            (None, "manual"),
        ):
            with self.subTest(outcome=outcome):
                runner = DevTunnelRunner(after_logout=outcome)
                service = self._service(runner)
                self.assertEqual(self._devtunnel_result(service, True).status, expected)
                self.assertEqual(self._verification(service).status, "manual verification required")

    def test_per_user_install_is_found_when_not_on_path(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "Programs" / "devtunnel" / "devtunnel.exe"
            executable.parent.mkdir(parents=True)
            executable.touch()
            with patch.dict(os.environ, {"LOCALAPPDATA": directory}), patch("browser_cleaner.developer_auth.shutil.which", return_value=None):
                service = DeveloperAuthService(runner=FakeRunner())
            self.assertEqual(service.devtunnel_command, str(executable))

    def test_unusable_executable_does_not_expose_errors_or_stop_other_cleanup(self):
        class UnusableRunner(DevTunnelRunner):
            def run(self, command):
                if command[0] == "devtunnel":
                    raise PermissionError("fixture-secret-value")
                return super().run(command)

        service = self._service(UnusableRunner())
        self.assertEqual(service.devtunnel_login_status(), "unknown")
        results = service.remove_supported(True, lambda _subject, _phrase: True)
        self.assertEqual(next(result for result in results if result.provider == "Microsoft Dev Tunnels").status, "error")
        self.assertTrue(any(result.status == "removed" for result in results))
        self.assertNotIn("fixture-secret-value", str(results))


if __name__ == "__main__":
    unittest.main()
