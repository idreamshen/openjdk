/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test
 * @bug 8199194
 * @summary smoke test for enable-preview command line flag
 * @modules jdk.compiler/com.sun.tools.javac.code
 */

import java.io.*;
import java.util.*;
import java.util.stream.Collectors;
import java.util.stream.Stream;

import com.sun.tools.javac.code.Source;

public class PreviewOptionTest {
    public static void main(String... args) throws Exception {
        PreviewOptionTest t = new PreviewOptionTest();
        t.run();
    }

    public void run() throws Exception {
        try (FileWriter out = new FileWriter("Test.java")) {
            out.write("class Test { }");
        }

        testWithNoFlags();

        List<Source> versionsToTest = Stream.of(Source.values())
                .filter(s -> s.compareTo(Source.MIN) >= 0)
                .collect(Collectors.toList());

        versionsToTest.stream().forEach(this::testWithSourceFlag);
        versionsToTest.stream().forEach(this::testWithReleaseFlag);

        if (errors > 0)
            throw new Exception(errors + " errors occurred");
    }

    void testWithNoFlags() {
        testInternal(null, null, true);
    }

    void testWithSourceFlag(Source source) {
        testInternal(source, null, source != Source.DEFAULT);
    }

    void testWithReleaseFlag(Source release) {
        //Todo: the condition below should say "release != Source.DEFAULT", but we can't do that
        //since --release 11 is not supported yet.
        testInternal(null, release, true);
    }

    void testInternal(Source source, Source release, boolean shouldFail) {
        System.err.println("Test: source:" + source + ", release:" + release + " " + shouldFail + " " + shouldFail);
        List<String> args = new ArrayList<>();
        args.add("--enable-preview");
        if (source != null) {
            args.add("-source");
            args.add(source.name);
        }
        if (release != null) {
            args.add("--release");
            args.add(release.name);
        }
        args.add("Test.java");

        StringWriter sw = new StringWriter();
        PrintWriter pw = new PrintWriter(sw);
        int rc = com.sun.tools.javac.Main.compile(args.toArray(new String[args.size()]), pw);
        pw.close();
        boolean hasErrors = rc != 0;
        if (hasErrors != shouldFail) {
            if (hasErrors) {
                String out = sw.toString();
                error("error not expected but found:\n" + out);
            } else {
                error("error expected but not found");
            }
        }
    }

    void error(String msg) {
        System.err.println("error: " + msg);
        errors++;
    }

    int errors;
}
